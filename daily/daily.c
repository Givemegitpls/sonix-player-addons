#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/store.h"
#include "src/addon/addon.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons_select.h"
#include "src/gui/shell/scale.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/power.h"
#include "src/system/library/library.h"
#include "src/system/playback/device_state.h"

#include "lvgl/lvgl.h"

// Daily — one track from the local library for the calendar day.
//
// The pick is hash(YYYY-MM-DD) into LIBRARY_LIST_TRACKS, skipping recent
// history when another track is available. Path/title/artist/album for today
// are kept in settings.ini so a library rebuild does not change today's pick.
//
// Same-day reopen uses the store + one MEDIA_TABLE lookup (no full tracks
// index). Cover decode goes through coverloader. The pick worker, when needed,
// runs SCHED_IDLE so it does not fight playback on the single core.
//
// Layout is a normal More-menu page: system topbar stays visible, title sits
// beside the floating back chevron (settingsrow), cover fills the body.

#define PATH_MAX_DAILY 512
#define HISTORY_SKIP 24
#define PICK_TRIES 24
#define PLAY_DISC SC(84) // same disc as the player's transport play button
#define META_GAP SC(12)
#define POLL_PERIOD_MS 500
// Gap between browser (0..9) and medialist (12..): addons do not share a
// reserved pool, so pick a slot nothing in the player claims.
#define DAILY_COVER_SLOT 10

typedef struct {
	char date[16];
	char path[PATH_MAX_DAILY];
	char name[256];
	char artist[256];
	char album[256];
	bool ok;
	bool no_library;
	bool no_tracks;
	unsigned gen;
} daily_job_t;

static const char *addon_id;
static gui_config_t *gui_cfg;

static lv_obj_t *daily_screen;
static lv_obj_t *daily_header_title;
static lv_obj_t *cover_frame;
static lv_obj_t *cover_img;
static lv_obj_t *title_label;
static lv_obj_t *artist_label;
static lv_obj_t *album_label;
static lv_obj_t *status_label;
static lv_obj_t *load_spinner;
static lv_obj_t *info_row;
static lv_obj_t *meta_col;
static lv_obj_t *play_btn;
static lv_obj_t *play_btn_icon;
static int cover_w;
static int cover_h;

static store_t store;
static bool store_ready;
static cover_image_t shown_cover;
static bool shown_has_cover;
static bool cover_pending;
static char shown_path[PATH_MAX_DAILY];
static char shown_date[16];
static unsigned job_gen;
static bool theme_hook;
static bool busy;
static bool play_shows_pause;
static lv_timer_t *poll_timer;

// True when today's pick is the loaded file (playing or paused).
static bool daily_is_loaded(const device_state_t *st) {
	return shown_path[0] && st && !st->live && st->current_file[0] &&
		   strcmp(st->current_file, shown_path) == 0;
}

static void set_play_icon(bool pause) {
	if (!play_btn_icon || play_shows_pause == pause) {
		return;
	}
	play_shows_pause = pause;
	lv_image_set_src(play_btn_icon, pause ? &icon_pause : &icon_play);
}

static void sync_play_btn(void) {
	if (!play_btn_icon || !shown_path[0]) {
		set_play_icon(false);
		return;
	}
	device_state_t st;
	device_state_get(&st);
	set_play_icon(daily_is_loaded(&st) && st.status == AUDIO_STATUS_PLAYING);
}

static bool ru(void) {
	const char *name = lang_current();
	return name && strcmp(name, "Russian") == 0;
}

// Cover under the page heading; leftover height holds meta + play disc.
static void compute_cover_size(void) {
	int bottom = PLAY_DISC + META_GAP + gui_cfg->padding;
	int top = settingsrow_content_top(gui_cfg);
	cover_w = (int)gui_cfg->screen_width;
	cover_h = cover_w; // prefer square
	int room = (int)gui_cfg->screen_height - top - bottom;
	if (cover_h > room) {
		cover_h = room > SC(160) ? room : SC(160);
	}
}

static void today_ymd(char *out, size_t size) {
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	if (strftime(out, size, "%Y-%m-%d", &tm) == 0 && size > 0) {
		out[0] = '\0';
	}
}

static uint32_t fnv1a(const char *s) {
	uint32_t h = 2166136261u;
	for (; s && *s; s++) {
		h ^= (uint8_t)*s;
		h *= 16777619u;
	}
	return h;
}

static void store_open(const char *addon_dir) {
	char path[STORE_PATH_MAX];
	snprintf(path, sizeof(path), "%s/settings.ini", addon_dir && addon_dir[0] ? addon_dir : "");
	store_init(&store, path);
	store_load(&store);
	store_ready = true;
}

static void persist_pick(const char *date, const char *path, const char *name, const char *artist, const char *album) {
	if (!store_ready) {
		return;
	}
	store_set(&store, "date", date);
	if (path && strlen(path) < STORE_VALUE_MAX) {
		store_set(&store, "path", path);
	} else {
		store_set(&store, "path", "");
	}
	store_set(&store, "name", name ? name : "");
	store_set(&store, "artist", artist ? artist : "");
	store_set(&store, "album", album ? album : "");
	store_save(&store);
}

typedef struct {
	char path[PATH_MAX_DAILY];
	char name[256];
	char artist[256];
	bool filled;
} row_catch_t;

static bool catch_row(const char *name, const char *path, const char *artist, void *user) {
	row_catch_t *c = user;
	snprintf(c->name, sizeof(c->name), "%s", name ? name : "");
	snprintf(c->path, sizeof(c->path), "%s", path ? path : "");
	snprintf(c->artist, sizeof(c->artist), "%s", artist ? artist : "");
	c->filled = c->path[0] != '\0';
	return false;
}

static bool path_in_recent(const char *path, const char *const *recent, int n) {
	if (!path || !path[0]) {
		return false;
	}
	for (int i = 0; i < n; i++) {
		if (recent[i] && strcmp(recent[i], path) == 0) {
			return true;
		}
	}
	return false;
}

static int load_recent_paths(char (*buf)[PATH_MAX_DAILY], int max, const char **ptrs) {
	library_history_item_t *items = NULL;
	int n = library_history_items(&items);
	if (n <= 0 || !items) {
		free(items);
		return 0;
	}
	int start = n > max ? n - max : 0;
	int out = 0;
	for (int i = start; i < n && out < max; i++) {
		char path[PATH_MAX_DAILY];
		char name[8];
		if (!library_history_entry(items[i].id, path, sizeof(path), name, sizeof(name))) {
			continue;
		}
		if (!path[0]) {
			continue;
		}
		snprintf(buf[out], PATH_MAX_DAILY, "%s", path);
		ptrs[out] = buf[out];
		out++;
	}
	free(items);
	return out;
}

static void fill_album_name(const char *path, char *album, size_t album_size) {
	album[0] = '\0';
	if (!path || !path[0]) {
		return;
	}
	char value[256];
	if (library_track_album_value(path, value, sizeof(value)) && value[0]) {
		library_album_title(value, album, album_size);
		if (!album[0]) {
			snprintf(album, album_size, "%s", value);
		}
		return;
	}
	library_track_album(path, album, album_size);
}

// Same calendar day already chosen: one MEDIA_TABLE lookup, no tracks index.
static bool pick_from_store(daily_job_t *job) {
	if (!store_ready || strcmp(store_get(&store, "date"), job->date) != 0) {
		return false;
	}
	const char *saved = store_get(&store, "path");
	if (!saved[0] || access(saved, F_OK) != 0) {
		return false;
	}
	if (!library_is_open()) {
		job->no_library = true;
		return false;
	}
	char title[256];
	if (!library_track_title(saved, title, sizeof(title))) {
		return false; // gone from the index — fall through to a fresh pick
	}

	snprintf(job->path, sizeof(job->path), "%s", saved);
	const char *name = store_get(&store, "name");
	snprintf(job->name, sizeof(job->name), "%s", name[0] ? name : title);
	snprintf(job->artist, sizeof(job->artist), "%s", store_get(&store, "artist"));
	snprintf(job->album, sizeof(job->album), "%s", store_get(&store, "album"));
	return true;
}

static bool pick_track(daily_job_t *job) {
	if (!library_is_open()) {
		job->no_library = true;
		return false;
	}

	if (pick_from_store(job)) {
		return true;
	}

	// New day (or stale store): build the full tracks index once.
	library_index_t *ix = library_index_open(LIBRARY_LIST_TRACKS, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT,
						 false);
	int count = library_index_count(ix);
	if (count <= 0) {
		library_index_close(ix);
		job->no_tracks = true;
		return false;
	}

	char (*recent_buf)[PATH_MAX_DAILY] = calloc((size_t)HISTORY_SKIP, PATH_MAX_DAILY);
	const char **recent_ptrs = calloc((size_t)HISTORY_SKIP, sizeof(*recent_ptrs));
	int recent_n = 0;
	if (recent_buf && recent_ptrs) {
		recent_n = load_recent_paths(recent_buf, HISTORY_SKIP, recent_ptrs);
	}

	uint32_t h = fnv1a(job->date);
	int start = (int)(h % (uint32_t)count);
	row_catch_t chosen = {0};
	bool found = false;

	for (int t = 0; t < PICK_TRIES && t < count; t++) {
		int idx = (start + t) % count;
		row_catch_t c = {0};
		library_index_window(ix, idx, 1, catch_row, &c);
		if (!c.filled) {
			continue;
		}
		if (recent_n > 0 && path_in_recent(c.path, recent_ptrs, recent_n) && t + 1 < count) {
			continue;
		}
		chosen = c;
		found = true;
		break;
	}
	if (!found) {
		library_index_window(ix, start, 1, catch_row, &chosen);
		found = chosen.filled;
	}
	library_index_close(ix);
	free(recent_buf);
	free(recent_ptrs);

	if (!found) {
		job->no_tracks = true;
		return false;
	}

	snprintf(job->path, sizeof(job->path), "%s", chosen.path);
	snprintf(job->name, sizeof(job->name), "%s", chosen.name);
	snprintf(job->artist, sizeof(job->artist), "%s", chosen.artist);
	fill_album_name(job->path, job->album, sizeof(job->album));
	persist_pick(job->date, job->path, job->name, job->artist, job->album);
	return true;
}

static void free_shown_cover(void) {
	if (shown_has_cover && cover_img) {
		lv_image_set_src(cover_img, NULL);
		cover_free(&shown_cover);
		memset(&shown_cover, 0, sizeof(shown_cover));
		shown_has_cover = false;
	}
}

static void set_cover_visible(bool has) {
	if (cover_img) {
		lv_obj_set_hidden(cover_img, !has);
	}
	if (cover_frame) {
		lv_obj_set_hidden(cover_frame, false);
	}
}

static void request_cover(const char *path) {
	cover_pending = false;
	coverloader_release(DAILY_COVER_SLOT);
	free_shown_cover();
	if (cover_img) {
		lv_obj_set_hidden(cover_img, true);
	}
	if (!path || !path[0]) {
		return;
	}
	int size = cover_w > cover_h ? cover_w : cover_h;
	coverloader_start();
	coverloader_request(DAILY_COVER_SLOT, path, size);
	cover_pending = true;
}

static void poll_cover(void) {
	if (!cover_pending || !cover_img) {
		return;
	}
	cover_image_t image;
	bool finished = false;
	bool found = coverloader_take(DAILY_COVER_SLOT, &image, &finished);
	if (!finished) {
		return;
	}
	cover_pending = false;
	if (!found) {
		set_cover_visible(false);
		return;
	}
	free_shown_cover();
	shown_cover = image;
	shown_has_cover = true;
	lv_image_set_src(cover_img, &shown_cover.dsc);
	set_cover_visible(true);
}

static void set_loading(bool on) {
	if (load_spinner) {
		lv_obj_set_hidden(load_spinner, !on);
	}
	if (!on) {
		return;
	}
	if (status_label) {
		lv_label_set_text(status_label, ru() ? "Загрузка…" : "Loading…");
		lv_obj_set_hidden(status_label, false);
	}
	if (info_row) {
		lv_obj_set_hidden(info_row, true);
	}
	if (cover_frame) {
		lv_obj_set_hidden(cover_frame, true);
	}
}

static void paint_empty(const char *msg) {
	cover_pending = false;
	coverloader_release(DAILY_COVER_SLOT);
	free_shown_cover();
	set_loading(false);
	if (cover_frame) {
		lv_obj_set_hidden(cover_frame, true);
	}
	if (info_row) {
		lv_obj_set_hidden(info_row, true);
	}
	lv_label_set_text(status_label, msg);
	lv_obj_set_hidden(status_label, false);
	shown_path[0] = '\0';
	shown_date[0] = '\0';
	set_play_icon(false);
}

static void paint_track(const char *date, const char *path, const char *name, const char *artist, const char *album) {
	set_loading(false);
	if (cover_frame) {
		lv_obj_set_hidden(cover_frame, false);
	}
	lv_label_set_text(title_label, name && name[0] ? name : path);
	lv_label_set_text(artist_label, artist ? artist : "");
	lv_obj_set_hidden(artist_label, !artist || artist[0] == '\0');
	if (album && album[0]) {
		lv_label_set_text(album_label, album);
		lv_obj_set_hidden(album_label, false);
	} else {
		lv_obj_set_hidden(album_label, true);
	}
	if (info_row) {
		lv_obj_set_hidden(info_row, false);
	}
	lv_obj_set_hidden(status_label, true);
	snprintf(shown_path, sizeof(shown_path), "%s", path ? path : "");
	snprintf(shown_date, sizeof(shown_date), "%s", date ? date : "");
	sync_play_btn();
	request_cover(shown_path);
}

static void apply_job(void *user) {
	daily_job_t *job = user;
	busy = false;
	if (!job) {
		return;
	}
	if (job->gen != job_gen || !daily_screen) {
		free(job);
		return;
	}

	if (!job->ok) {
		const char *msg;
		if (job->no_library) {
			msg = ru() ? "Сначала просканируйте\nбиблиотеку в Music." : "Scan the library\nunder Music first.";
		} else if (job->no_tracks) {
			msg = ru() ? "В библиотеке нет треков." : "No tracks in the library.";
		} else {
			msg = ru() ? "Не удалось выбрать трек." : "Could not pick a track.";
		}
		paint_empty(msg);
		free(job);
		return;
	}

	paint_track(job->date, job->path, job->name, job->artist, job->album);
	free(job);
}

static void *refresh_worker(void *arg) {
	daily_job_t *job = arg;
	thread_be_background("daily");
	job->ok = pick_track(job);
	if (!gui_post(apply_job, job)) {
		free(job);
		busy = false;
	}
	return NULL;
}

static bool already_showing_today(void) {
	char today[16];
	today_ymd(today, sizeof(today));
	return shown_path[0] && shown_date[0] && strcmp(shown_date, today) == 0 &&
		   store_ready && strcmp(store_get(&store, "path"), shown_path) == 0;
}

// Instant path: settings.ini already holds today and the file is still indexed.
static bool try_apply_stored_today(void) {
	daily_job_t job = {0};
	today_ymd(job.date, sizeof(job.date));
	if (!pick_from_store(&job)) {
		return false;
	}
	job.ok = true;
	paint_track(job.date, job.path, job.name, job.artist, job.album);
	return true;
}

static void start_refresh(void) {
	if (busy) {
		return;
	}
	if (already_showing_today()) {
		sync_play_btn();
		return;
	}

	// Same day in store: paint on the UI thread, cover via coverloader.
	if (try_apply_stored_today()) {
		return;
	}

	daily_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	today_ymd(job->date, sizeof(job->date));
	job->gen = ++job_gen;
	busy = true;
	set_loading(true);

	pthread_t th;
	if (pthread_create(&th, NULL, refresh_worker, job) != 0) {
		busy = false;
		free(job);
		paint_empty(ru() ? "Ошибка потока." : "Thread error.");
		return;
	}
	pthread_detach(th);
}

static void poll_cb(lv_timer_t *t) {
	(void)t;
	if (!daily_screen || lv_screen_active() != daily_screen) {
		return;
	}
	poll_cover();
	sync_play_btn();
}

static void play_cb(lv_event_t *e) {
	(void)e;
	if (!shown_path[0] || switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	device_state_t st;
	device_state_get(&st);
	if (daily_is_loaded(&st)) {
		bool was_playing = st.status == AUDIO_STATUS_PLAYING;
		player_key_play_pause();
		set_play_icon(!was_playing);
		return;
	}
	player_play_file(shown_path);
	set_play_icon(true);
}

static void theme_cb(void) {
	if (cover_frame) {
		lv_obj_set_style_bg_color(cover_frame, theme()->cover_bg, 0);
	}
	if (title_label) {
		lv_obj_set_style_text_color(title_label, theme()->text_primary, 0);
	}
	if (artist_label) {
		lv_obj_set_style_text_color(artist_label, theme()->text_secondary, 0);
	}
	if (album_label) {
		lv_obj_set_style_text_color(album_label, theme()->text_secondary, 0);
	}
	if (status_label) {
		lv_obj_set_style_text_color(status_label, theme()->text_secondary, 0);
	}
	if (play_btn) {
		lv_obj_set_style_bg_color(play_btn, lv_color_white(), 0);
	}
	if (play_btn_icon) {
		lv_obj_set_style_image_recolor(play_btn_icon, theme()->accent, 0);
		lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);
	}
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	if (poll_timer) {
		lv_timer_resume(poll_timer);
	}
	sync_play_btn();
	start_refresh();
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	if (poll_timer) {
		lv_timer_pause(poll_timer);
	}
}

static lv_obj_t *daily_addon_init(const sonix_addon_host_t *host) {
	gui_cfg = host->cfg;
	addon_id = host->id;
	store_open(host->dir);
	compute_cover_size();
	coverloader_start();

	daily_screen = lv_obj_create(NULL);
	lv_obj_add_style(daily_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(daily_screen, false);
	sonix_addon_own_screen(addon_id, daily_screen);

	daily_header_title = settingsrow_title(daily_screen, gui_cfg, ru() ? "Трек дня" : "Daily");

	int sw = (int)host->cfg->screen_width;
	int pad = host->cfg->padding;
	int top = settingsrow_content_top(gui_cfg);

	cover_frame = lv_obj_create(daily_screen);
	lv_obj_remove_style_all(cover_frame);
	lv_obj_set_pos(cover_frame, 0, top);
	lv_obj_set_size(cover_frame, cover_w, cover_h);
	lv_obj_set_style_radius(cover_frame, 0, 0);
	lv_obj_set_style_clip_corner(cover_frame, true, 0);
	lv_obj_set_style_bg_color(cover_frame, theme()->cover_bg, 0);
	lv_obj_set_style_bg_opa(cover_frame, LV_OPA_COVER, 0);
	lv_obj_set_hidden(cover_frame, true);

	cover_img = lv_image_create(cover_frame);
	lv_obj_set_size(cover_img, cover_w, cover_h);
	lv_obj_center(cover_img);
	lv_obj_set_hidden(cover_img, true);

	int info_y = top + cover_h + META_GAP;
	info_row = lv_obj_create(daily_screen);
	lv_obj_remove_style_all(info_row);
	lv_obj_set_pos(info_row, pad, info_y);
	lv_obj_set_size(info_row, sw - 2 * pad, PLAY_DISC);
	lv_obj_set_flex_flow(info_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(info_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(info_row, SC(12), 0);
	lv_obj_set_scrollable(info_row, false);
	lv_obj_set_hidden(info_row, true);

	meta_col = lv_obj_create(info_row);
	lv_obj_remove_style_all(meta_col);
	lv_obj_set_height(meta_col, LV_SIZE_CONTENT);
	lv_obj_set_width(meta_col, 0);
	lv_obj_set_flex_grow(meta_col, 1);
	lv_obj_set_flex_flow(meta_col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(meta_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(meta_col, SC(2), 0);
	lv_obj_set_scrollable(meta_col, false);

	title_label = lv_label_create(meta_col);
	lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_style_text_font(title_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(title_label, theme()->text_primary, 0);
	lv_label_set_text(title_label, "");

	artist_label = lv_label_create(meta_col);
	lv_label_set_long_mode(artist_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(artist_label, lv_pct(100));
	lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_style_text_font(artist_label, &font_ui_20, 0);
	lv_obj_set_style_text_color(artist_label, theme()->text_secondary, 0);
	lv_label_set_text(artist_label, "");

	album_label = lv_label_create(meta_col);
	lv_label_set_long_mode(album_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(album_label, lv_pct(100));
	lv_obj_set_style_text_align(album_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_style_text_font(album_label, &font_ui_18, 0);
	lv_obj_set_style_text_color(album_label, theme()->text_secondary, 0);
	lv_label_set_text(album_label, "");
	lv_obj_set_hidden(album_label, true);

	play_btn = lv_btn_create(info_row);
	lv_obj_set_size(play_btn, PLAY_DISC, PLAY_DISC);
	lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(play_btn, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_shadow_width(play_btn, 0, 0);
	lv_obj_set_style_border_width(play_btn, 0, 0);
	lv_obj_add_event_cb(play_btn, play_cb, LV_EVENT_CLICKED, NULL);

	play_btn_icon = lv_image_create(play_btn);
	play_shows_pause = false;
	lv_image_set_src(play_btn_icon, &icon_play);
	lv_obj_set_style_image_recolor(play_btn_icon, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);
	lv_obj_center(play_btn_icon);

	load_spinner = spinner_create(daily_screen, &icon_loader_big);
	lv_obj_align(load_spinner, LV_ALIGN_CENTER, 0, SC(-24));
	lv_obj_set_hidden(load_spinner, true);

	status_label = lv_label_create(daily_screen);
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(status_label, sw - 2 * pad);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_18, 0);
	lv_obj_set_style_text_color(status_label, theme()->text_secondary, 0);
	lv_label_set_text(status_label, "");
	lv_obj_align(status_label, LV_ALIGN_CENTER, 0, SC(36));
	lv_obj_set_hidden(status_label, true);

	lv_obj_add_event_cb(daily_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(daily_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(daily_screen);
	player_sheet_attach_drag(daily_screen, true);
	player_sheet_attach_drag(cover_frame, true);
	player_sheet_attach_drag(info_row, true);

	poll_timer = lv_timer_create(poll_cb, POLL_PERIOD_MS, NULL);
	lv_timer_pause(poll_timer);
	power_pause_in_standby(poll_timer);

	if (!theme_hook) {
		theme_register_refresh(theme_cb);
		theme_hook = true;
	}
	return daily_screen;
}

static bool daily_addon_stop(void) {
	job_gen++;
	cover_pending = false;
	coverloader_release(DAILY_COVER_SLOT);
	if (poll_timer) {
		power_forget_timer(poll_timer);
		lv_timer_delete(poll_timer);
		poll_timer = NULL;
	}
	if (theme_hook) {
		theme_unregister_refresh(theme_cb);
		theme_hook = false;
	}
	if (cover_img) {
		free_shown_cover();
	}
	if (daily_screen) {
		switcher_forget_screen(daily_screen);
	}

	daily_screen = NULL;
	daily_header_title = NULL;
	cover_frame = NULL;
	cover_img = NULL;
	title_label = NULL;
	artist_label = NULL;
	album_label = NULL;
	status_label = NULL;
	load_spinner = NULL;
	info_row = NULL;
	meta_col = NULL;
	play_btn = NULL;
	play_btn_icon = NULL;
	gui_cfg = NULL;
	addon_id = NULL;
	busy = false;
	play_shows_pause = false;
	shown_path[0] = '\0';
	shown_date[0] = '\0';
	return true;
}

static const sonix_addon_t daily = {
	.api = SONIX_ADDON_API,
	.title = "Daily",
	.init = daily_addon_init,
};

static const sonix_addon_stop_t daily_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = daily_addon_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &daily : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &daily_lifecycle : NULL;
}
