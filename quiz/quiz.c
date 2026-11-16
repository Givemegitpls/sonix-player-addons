#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/store.h"
#include "src/addon/addon.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons_select.h"
#include "src/gui/shell/scale.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/power.h"
#include "src/system/image/stb_image_decl.h"
#include "src/system/library/library.h"
#include "src/system/playback/device_state.h"

#include "lvgl/lvgl.h"

// Quiz — library snippet, four choices. Three play modes (Survival / Endless /
// Timed), a start card to pick ask+mode, settings behind the gear, and personal
// bests behind the trophy.

#define PATH_MAX_QUIZ 512
#define CHOICES 4
#define POOL_MAX 128
#define SNIPPET_MS 12000
#define SEEK_SETTLE_MS 500
#define ANSWER_TICK_MS 200
#define CHOICE_H SC(80)
#define CHOICE_GAP SC(8)
#define TOOL_H SC(64)
#define AUTO_NEXT_MS 700
#define FEEDBACK_HALF_MS 120
#define FEEDBACK_FLASHES 3
#define FEEDBACK_SOLID_MS 550
#define QUIZ_COVER_SLOT 11
#define HEART_PX SC(26)
#define HEART_GAP SC(6)
// Wide enough for "30 / 120" in font_ui_26 so the lives row does not slide.
#define TIMER_SLOT_W SC(140)

typedef enum { MODE_ARTIST = 0, MODE_TRACK = 1, MODE_ALBUM = 2, MODE_COUNT } quiz_mode_t;
typedef enum { PLAY_SURVIVAL = 0, PLAY_ENDLESS = 1, PLAY_TIMED = 2, PLAY_COUNT } play_mode_t;

typedef struct {
	char path[PATH_MAX_QUIZ];
	char name[256];
	char artist[256];
	char album[256];
} quiz_track_t;

typedef struct {
	quiz_track_t tracks[CHOICES];
	int correct;
	quiz_mode_t mode;
	bool ok;
	bool no_library;
	bool no_tracks;
	unsigned gen;
} quiz_job_t;

typedef struct {
	quiz_mode_t ask;
	play_mode_t play;
	bool reveal_on;
	int reveal_sec;
	int survival_lives; // 1..5
	int timed_run_sec;
	int timed_limit_sec;
	int timed_lives; // 1..5
} quiz_cfg_t;

static const int LIMIT_SECS[] = {5, 10, 15, 20, 25, 30};
static const int RUN_SECS[] = {30, 45, 60, 90, 120};
static const int REVEAL_SECS[] = {1, 2, 3, 4, 5};
static const int LIVES_N_OPTS[] = {1, 2, 3, 4, 5};
#define LIMIT_N ((int)(sizeof(LIMIT_SECS) / sizeof(LIMIT_SECS[0])))
#define RUN_N ((int)(sizeof(RUN_SECS) / sizeof(RUN_SECS[0])))
#define REVEAL_N ((int)(sizeof(REVEAL_SECS) / sizeof(REVEAL_SECS[0])))
#define LIVES_N ((int)(sizeof(LIVES_N_OPTS) / sizeof(LIVES_N_OPTS[0])))
#define LIVES_MAX 5

static const char *addon_id;
static const char *addon_dir;
static gui_config_t *gui_cfg;
static store_t store;
static bool store_ready;
static quiz_cfg_t cfg;
static quiz_cfg_t cfg_before_settings; // snapshot when the gear opens

static lv_obj_t *quiz_screen;
static lv_obj_t *settings_screen;
static lv_obj_t *records_screen;
static lv_obj_t *records_surv_val;
static lv_obj_t *records_end_val;
static lv_obj_t *records_timed_val;
static lv_obj_t *prompt_label;
static lv_obj_t *meta_row;
static lv_obj_t *score_label;
static lv_obj_t *lives_row;
static lv_obj_t *lives_img[LIVES_MAX];
static lv_obj_t *timer_label;
static lv_obj_t *feedback_label;
static lv_obj_t *choice_btn[CHOICES];
static lv_obj_t *choice_lbl[CHOICES];
static lv_obj_t *replay_btn;
static lv_obj_t *replay_lbl;
static lv_obj_t *idle_box;
static lv_obj_t *idle_title;
static lv_obj_t *idle_ask_cap;
static lv_obj_t *idle_ask_row;
static lv_obj_t *idle_ask_pills[MODE_COUNT];
static lv_obj_t *idle_play_cap;
static lv_obj_t *idle_play_row;
static lv_obj_t *idle_play_pills[PLAY_COUNT];
static lv_obj_t *idle_mode_blurb;
static lv_obj_t *start_btn;
static lv_obj_t *start_lbl;
static lv_obj_t *load_spinner;
static lv_obj_t *status_label;
static lv_obj_t *reveal_box;
static lv_obj_t *reveal_cover_img;
static lv_obj_t *reveal_result;
static lv_obj_t *reveal_title;
static lv_obj_t *reveal_meta;
static int reveal_cover_px;

static lv_obj_t *results_box;
static lv_obj_t *results_card;
static lv_obj_t *results_title;
static lv_obj_t *results_score;
static lv_obj_t *results_score_cap;
static lv_obj_t *results_stats_row;
static lv_obj_t *results_stat_correct;
static lv_obj_t *results_stat_correct_val;
static lv_obj_t *results_stat_correct_lbl;
static lv_obj_t *results_stat_lives;
static lv_obj_t *results_stat_lives_val;
static lv_obj_t *results_stat_lives_lbl;
static lv_obj_t *results_btn;
static lv_obj_t *results_btn_lbl;

static lv_image_dsc_t heart_full_dsc;
static lv_image_dsc_t heart_empty_dsc;
static void *heart_full_px;
static void *heart_empty_px;
static bool hearts_ready;
static lv_image_dsc_t trophy_dsc;
static void *trophy_px;
static bool trophy_ready;
static lv_obj_t *trophy_icon;

static int best_survival_score;
static int best_endless_streak;
static int best_timed_score;

static lv_obj_t *set_ask_pills[MODE_COUNT];
static lv_obj_t *set_play_pills[PLAY_COUNT];
static lv_obj_t *set_survival_box;
static lv_obj_t *set_survival_lives_card;
static lv_obj_t *set_survival_lives_val;
static lv_obj_t *set_survival_lives_slider;
static lv_obj_t *set_timed_box;
static lv_obj_t *set_timed_run_card;
static lv_obj_t *set_timed_run_val;
static lv_obj_t *set_timed_run_slider;
static lv_obj_t *set_timed_limit_card;
static lv_obj_t *set_timed_limit_val;
static lv_obj_t *set_timed_limit_slider;
static lv_obj_t *set_timed_lives_card;
static lv_obj_t *set_timed_lives_val;
static lv_obj_t *set_timed_lives_slider;
static lv_obj_t *reveal_card;
static lv_obj_t *reveal_sw;
static lv_obj_t *reveal_val;
static lv_obj_t *reveal_slider;

static quiz_track_t pool[POOL_MAX];
static int pool_n;
static bool pool_ready;
static bool pool_used[POOL_MAX]; // correct-answer tracks already played this game
static bool pool_failed_library;
static bool pool_failed_tracks;

static quiz_track_t round_tracks[CHOICES];
static int round_correct = -1;
static bool answered;
static bool busy;
static bool revealing;
static unsigned job_gen;
static int score;
static int streak;		  // Endless: correct answers in a row
static int best_streak;	  // Endless: best this session (for results if ever shown)
static int run_correct;	  // correct answers in the current/last ended session
static bool run_end_failed; // lives out vs time simply ran out
static int lives_left;
static bool choice_missed[CHOICES];
static bool game_started;
static bool theme_hook;

static lv_timer_t *snippet_timer;
static lv_timer_t *seek_timer;
static lv_timer_t *answer_timer;
static lv_timer_t *reveal_timer;
static lv_timer_t *feedback_timer;
static int feedback_toggles_left; // remaining on/off flips before solid
static bool feedback_blink_lit;
static bool feedback_holding;	  // solid colour, waiting to advance
static int feedback_ok_idx = -1;
static bool we_started_playback;
static char snippet_path[PATH_MAX_QUIZ];
static uint32_t answer_deadline_ms;
static int answer_left_ms;

static bool run_active;
static bool run_needs_again;
static uint32_t run_deadline_ms;
static int run_left_ms;
static char last_result_line[320];

static cover_image_t reveal_cover;
static bool reveal_has_cover;
static bool reveal_cover_pending;

static bool ru(void) {
	const char *name = lang_current();
	return name && strcmp(name, "Russian") == 0;
}

static int nearest_index(const int *vals, int n, int v) {
	int best = 0;
	int best_d = abs(vals[0] - v);
	for (int i = 1; i < n; i++) {
		int d = abs(vals[i] - v);
		if (d < best_d) {
			best = i;
			best_d = d;
		}
	}
	return best;
}

static void cfg_defaults(void) {
	cfg.ask = MODE_ARTIST;
	cfg.play = PLAY_SURVIVAL;
	cfg.reveal_on = true;
	cfg.reveal_sec = 3;
	cfg.survival_lives = 3;
	cfg.timed_run_sec = 60;
	cfg.timed_limit_sec = 15;
	cfg.timed_lives = 3;
}

static void cfg_load(void) {
	cfg_defaults();
	if (!store_ready) {
		return;
	}
	int ask = store_get_int(&store, "ask", (int)MODE_ARTIST);
	if (ask < 0 || ask >= MODE_COUNT) {
		ask = MODE_ARTIST;
	}
	cfg.ask = (quiz_mode_t)ask;

	int play = store_get_int(&store, "play", -1);
	if (play < 0 || play >= PLAY_COUNT) {
		// Migrate old toggles: timed challenge → Timed, else Survival.
		play = store_get_int(&store, "run_on", 0) != 0 ? (int)PLAY_TIMED : (int)PLAY_SURVIVAL;
	}
	cfg.play = (play_mode_t)play;

	cfg.reveal_on = store_get_int(&store, "reveal_on", 1) != 0;
	cfg.reveal_sec = REVEAL_SECS[nearest_index(REVEAL_SECS, REVEAL_N, store_get_int(&store, "reveal_sec", 3))];

	int legacy_lives = store_get_int(&store, "lives", 3);
	cfg.survival_lives =
		LIVES_N_OPTS[nearest_index(LIVES_N_OPTS, LIVES_N, store_get_int(&store, "survival_lives", legacy_lives))];
	cfg.timed_lives =
		LIVES_N_OPTS[nearest_index(LIVES_N_OPTS, LIVES_N, store_get_int(&store, "timed_lives", legacy_lives))];
	cfg.timed_run_sec = RUN_SECS[nearest_index(RUN_SECS, RUN_N, store_get_int(&store, "timed_run_sec",
																			  store_get_int(&store, "run_sec", 60)))];
	cfg.timed_limit_sec = LIMIT_SECS[nearest_index(
		LIMIT_SECS, LIMIT_N, store_get_int(&store, "timed_limit_sec", store_get_int(&store, "limit_sec", 15)))];

	best_survival_score = store_get_int(&store, "best_survival_score", 0);
	best_endless_streak = store_get_int(&store, "best_endless_streak", 0);
	best_timed_score = store_get_int(&store, "best_timed_score", 0);
	if (best_survival_score < 0) {
		best_survival_score = 0;
	}
	if (best_endless_streak < 0) {
		best_endless_streak = 0;
	}
	if (best_timed_score < 0) {
		best_timed_score = 0;
	}

	// Drop migrated keys so STORE_KEYS room stays for records + settings.
	store_set(&store, "run_on", "");
	store_set(&store, "limit_on", "");
	store_set(&store, "lives", "");
	store_set(&store, "run_sec", "");
	store_set(&store, "limit_sec", "");
}

static void cfg_save(void) {
	if (!store_ready) {
		return;
	}
	store_set_int(&store, "ask", (int)cfg.ask);
	store_set_int(&store, "play", (int)cfg.play);
	store_set_int(&store, "reveal_on", cfg.reveal_on ? 1 : 0);
	store_set_int(&store, "reveal_sec", cfg.reveal_sec);
	store_set_int(&store, "survival_lives", cfg.survival_lives);
	store_set_int(&store, "timed_lives", cfg.timed_lives);
	store_set_int(&store, "timed_run_sec", cfg.timed_run_sec);
	store_set_int(&store, "timed_limit_sec", cfg.timed_limit_sec);
	store_set_int(&store, "best_survival_score", best_survival_score);
	store_set_int(&store, "best_endless_streak", best_endless_streak);
	store_set_int(&store, "best_timed_score", best_timed_score);
	store_save(&store);
}

static void records_save(void) {
	if (!store_ready) {
		return;
	}
	store_set_int(&store, "best_survival_score", best_survival_score);
	store_set_int(&store, "best_endless_streak", best_endless_streak);
	store_set_int(&store, "best_timed_score", best_timed_score);
	store_save(&store);
}

static bool records_note_survival(int points) {
	if (points <= best_survival_score) {
		return false;
	}
	best_survival_score = points;
	records_save();
	return true;
}

static bool records_note_timed(int points) {
	if (points <= best_timed_score) {
		return false;
	}
	best_timed_score = points;
	records_save();
	return true;
}

static bool records_note_endless(int streak_n) {
	if (streak_n <= best_endless_streak) {
		return false;
	}
	best_endless_streak = streak_n;
	records_save();
	return true;
}

static void records_clear(void) {
	best_survival_score = 0;
	best_endless_streak = 0;
	best_timed_score = 0;
	records_save();
}

static bool play_has_lives(void) {
	return cfg.play == PLAY_SURVIVAL || cfg.play == PLAY_TIMED;
}

static bool play_has_session_clock(void) {
	return cfg.play == PLAY_TIMED;
}

static bool play_has_answer_clock(void) {
	return cfg.play == PLAY_TIMED;
}

static int cfg_lives(void) {
	if (cfg.play == PLAY_SURVIVAL) {
		return cfg.survival_lives;
	}
	if (cfg.play == PLAY_TIMED) {
		return cfg.timed_lives;
	}
	return 0;
}

static void store_open(const char *dir) {
	char path[STORE_PATH_MAX];
	snprintf(path, sizeof(path), "%s/settings.ini", dir && dir[0] ? dir : "");
	store_init(&store, path);
	store_load(&store);
	store_ready = true;
	cfg_load();
}

static const char *mode_prompt(quiz_mode_t m) {
	if (ru()) {
		switch (m) {
		case MODE_TRACK:
			return "Какой трек?";
		case MODE_ALBUM:
			return "Какой альбом?";
		default:
			return "Кто исполнитель?";
		}
	}
	switch (m) {
	case MODE_TRACK:
		return "Which track?";
	case MODE_ALBUM:
		return "Which album?";
	default:
		return "Who is the artist?";
	}
}

static const char *track_label(const quiz_track_t *t, quiz_mode_t m) {
	switch (m) {
	case MODE_TRACK:
		return t->name[0] ? t->name : (t->path[0] ? t->path : "?");
	case MODE_ALBUM:
		return t->album[0] ? t->album : (t->artist[0] ? t->artist : "?");
	default:
		return t->artist[0] ? t->artist : (t->name[0] ? t->name : "?");
	}
}

static bool label_used(const quiz_track_t *tracks, int n, quiz_mode_t m, const quiz_track_t *cand) {
	const char *lab = track_label(cand, m);
	if (!lab || !lab[0] || strcmp(lab, "?") == 0) {
		return true;
	}
	for (int i = 0; i < n; i++) {
		if (strcmp(track_label(&tracks[i], m), lab) == 0) {
			return true;
		}
	}
	return false;
}

static void stop_answer_timer(void) {
	if (answer_timer) {
		lv_timer_pause(answer_timer);
	}
	answer_left_ms = 0;
	// Leave run_left_ms alone: finish_answer still reads it to decide whether
	// a timed run has expired. Clearing the label is enough for the UI.
	if (timer_label) {
		lv_label_set_text(timer_label, "");
	}
}

static void stop_snippet(void) {
	if (snippet_timer) {
		lv_timer_pause(snippet_timer);
	}
	if (seek_timer) {
		lv_timer_pause(seek_timer);
	}
	if (we_started_playback) {
		device_state_stop();
		we_started_playback = false;
	}
}

static void free_reveal_cover(void) {
	reveal_cover_pending = false;
	coverloader_release(QUIZ_COVER_SLOT);
	if (reveal_has_cover && reveal_cover_img) {
		lv_image_set_src(reveal_cover_img, NULL);
		cover_free(&reveal_cover);
		memset(&reveal_cover, 0, sizeof(reveal_cover));
		reveal_has_cover = false;
	}
}

static void hide_reveal(void) {
	revealing = false;
	if (reveal_timer) {
		lv_timer_pause(reveal_timer);
	}
	free_reveal_cover();
	if (reveal_box) {
		lv_obj_set_hidden(reveal_box, true);
	}
}

static void hide_results(void) {
	if (results_box) {
		lv_obj_set_hidden(results_box, true);
	}
}

static void compute_reveal_cover(void) {
	if (!gui_cfg) {
		reveal_cover_px = SC(240);
		return;
	}
	int side = (int)gui_cfg->screen_width - 2 * (int)gui_cfg->padding;
	int max_h = (int)gui_cfg->screen_height / 2;
	if (side > max_h) {
		side = max_h;
	}
	if (side < SC(160)) {
		side = SC(160);
	}
	reveal_cover_px = side;
}

static void schedule_continue(int ms) {
	if (!reveal_timer) {
		return;
	}
	if (ms < 1) {
		ms = 1;
	}
	lv_timer_set_period(reveal_timer, ms);
	lv_timer_reset(reveal_timer);
	lv_timer_resume(reveal_timer);
}

static void snippet_done_cb(lv_timer_t *t) {
	(void)t;
	if (we_started_playback) {
		device_state_stop();
		we_started_playback = false;
	}
}

static double snippet_offset(double total) {
	if (total < 18.0) {
		return 0.0;
	}
	if (total < 50.0) {
		return total * 0.25;
	}
	double mid = total * 0.35;
	double leave = (double)SNIPPET_MS / 1000.0 + 2.0;
	if (mid + leave > total) {
		mid = total - leave;
	}
	return mid > 0.0 ? mid : 0.0;
}

static void seek_settle_cb(lv_timer_t *t) {
	(void)t;
	if (seek_timer) {
		lv_timer_pause(seek_timer);
	}
	if (!we_started_playback || !snippet_path[0]) {
		return;
	}
	device_state_t st;
	device_state_get(&st);
	double off = snippet_offset(st.progress_total_secs);
	if (off > 0.5) {
		device_state_seek(off);
	}
	if (snippet_timer) {
		lv_timer_reset(snippet_timer);
		lv_timer_resume(snippet_timer);
	}
}

static void start_snippet(const char *path) {
	stop_snippet();
	if (!path || !path[0]) {
		return;
	}
	snprintf(snippet_path, sizeof(snippet_path), "%s", path);
	device_state_play_file(path);
	we_started_playback = true;
	if (seek_timer) {
		lv_timer_reset(seek_timer);
		lv_timer_resume(seek_timer);
	}
}

static bool per_answer_timer_active(void) {
	return play_has_answer_clock();
}

static int per_answer_ms(void) {
	return cfg.timed_limit_sec * 1000;
}

// White heart PNGs from assets/, tinted to the theme accent (same path as
// cassette sprites: card file → stb → ARGB8888 kept for the page lifetime).
static bool heart_sprite_load(const char *name, lv_image_dsc_t *dsc, void **keep) {
	char path[PATH_MAX_QUIZ];
	snprintf(path, sizeof(path), "%s/assets/%s", addon_dir ? addon_dir : "", name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long len = ftell(f);
	if (len <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}
	uint8_t *buf = malloc((size_t)len);
	if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return false;
	}
	fclose(f);

	int w = 0, h = 0, n = 0;
	uint8_t *px = stbi_load_from_memory(buf, (int)len, &w, &h, &n, 4);
	free(buf);
	if (!px || w <= 0 || h <= 0) {
		stbi_image_free(px);
		return false;
	}
	// stb is RGBA; LVGL ARGB8888 on this target is BGRA in memory.
	for (int i = 0; i < w * h; i++) {
		uint8_t r = px[i * 4 + 0], b = px[i * 4 + 2];
		px[i * 4 + 0] = b;
		px[i * 4 + 2] = r;
	}

	memset(dsc, 0, sizeof(*dsc));
	dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
	dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
	dsc->header.w = (uint32_t)w;
	dsc->header.h = (uint32_t)h;
	dsc->header.stride = (uint32_t)w * 4;
	dsc->data = px;
	dsc->data_size = (uint32_t)(w * h * 4);
	*keep = px;
	return true;
}

static void heart_tint(lv_obj_t *img, bool filled) {
	if (!img) {
		return;
	}
	lv_obj_set_style_image_recolor(img, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(img, filled ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void paint_lives(void) {
	if (!lives_row) {
		return;
	}
	int total = cfg_lives();
	if (total > LIVES_MAX) {
		total = LIVES_MAX;
	}
	// Hearts only in Survival/Timed with 2+ lives.
	if (!game_started || !hearts_ready || !play_has_lives() || total <= 1) {
		lv_obj_set_hidden(lives_row, true);
		return;
	}
	for (int i = 0; i < LIVES_MAX; i++) {
		lv_obj_t *img = lives_img[i];
		if (!img) {
			continue;
		}
		if (i >= total) {
			lv_obj_set_hidden(img, true);
			continue;
		}
		bool filled = (i < lives_left);
		lv_image_set_src(img, filled ? &heart_full_dsc : &heart_empty_dsc);
		heart_tint(img, filled);
		lv_obj_set_hidden(img, false);
	}
	lv_obj_set_hidden(lives_row, false);
}

static void paint_score(void) {
	if (!score_label) {
		return;
	}
	if (!game_started) {
		lv_label_set_text(score_label, "");
		paint_lives();
		return;
	}
	char line[64];
	if (cfg.play == PLAY_ENDLESS) {
		snprintf(line, sizeof(line), ru() ? "Серия %d" : "Streak %d", streak);
	} else {
		snprintf(line, sizeof(line), ru() ? "Счёт %d" : "Score %d", score);
	}
	lv_label_set_text(score_label, line);
	paint_lives();
}

static void paint_timer(void) {
	if (!timer_label) {
		return;
	}
	if (answered || revealing) {
		lv_label_set_text(timer_label, "");
		return;
	}
	char line[48];
	if (play_has_session_clock() && run_active && run_left_ms > 0) {
		int rs = (run_left_ms + 999) / 1000;
		if (per_answer_timer_active() && answer_left_ms > 0) {
			snprintf(line, sizeof(line), "%2d / %3d", (answer_left_ms + 999) / 1000, rs);
		} else {
			snprintf(line, sizeof(line), "%3d", rs);
		}
	} else if (per_answer_timer_active() && answer_left_ms > 0) {
		snprintf(line, sizeof(line), "%2d", (answer_left_ms + 999) / 1000);
	} else {
		line[0] = '\0';
	}
	lv_label_set_text(timer_label, line);
}

// Soft fills with black label text — clearer than mid neon + white.
static lv_color_t color_ok(bool lit) {
	if (theme()->dark) {
		return lit ? lv_color_make(0x5C, 0xC9, 0x9A) : lv_color_make(0x45, 0xA8, 0x7E);
	}
	return lit ? lv_color_make(0x6F, 0xD4, 0xA8) : lv_color_make(0x8A, 0xDE, 0xB8);
}

static lv_color_t color_miss(void) {
	if (theme()->dark) {
		return lv_color_make(0xE0, 0x8A, 0x8E);
	}
	return lv_color_make(0xE8, 0x9A, 0x9E);
}

static void style_choice(lv_obj_t *btn, bool accent, bool dim) {
	if (!btn) {
		return;
	}
	if (accent) {
		lv_obj_set_style_bg_color(btn, theme()->accent, 0);
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	} else if (dim) {
		lv_obj_set_style_bg_color(btn, theme()->surface, 0);
		lv_obj_set_style_bg_opa(btn, LV_OPA_40, 0);
	} else {
		lv_obj_set_style_bg_color(btn, theme()->surface, 0);
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	}
}

static void style_choice_ok(lv_obj_t *btn, int index, bool lit) {
	if (!btn) {
		return;
	}
	lv_obj_set_style_bg_color(btn, color_ok(lit), 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	if (index >= 0 && index < CHOICES && choice_lbl[index]) {
		lv_obj_set_style_text_color(choice_lbl[index], lv_color_black(), 0);
	}
}

static void style_choice_miss(lv_obj_t *btn, int index) {
	if (!btn) {
		return;
	}
	lv_obj_set_style_bg_color(btn, color_miss(), 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	if (index >= 0 && index < CHOICES && choice_lbl[index]) {
		lv_obj_set_style_text_color(choice_lbl[index], lv_color_black(), 0);
	}
}

// Mid-run wrong pick: stay on the grey surface, just quieter — not a loss colour.
static void style_choice_rejected(lv_obj_t *btn, int index) {
	if (!btn) {
		return;
	}
	lv_obj_set_style_bg_color(btn, theme()->surface, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_40, 0);
	if (index >= 0 && index < CHOICES && choice_lbl[index]) {
		lv_obj_set_style_text_color(choice_lbl[index], theme()->text_secondary, 0);
	}
}

// Keep the replay row in the flex layout so choices do not jump when it
// becomes unavailable — fade + disable instead of hide.
static void replay_set_available(bool on) {
	if (!replay_btn) {
		return;
	}
	lv_obj_set_hidden(replay_btn, false);
	if (on) {
		lv_obj_remove_state(replay_btn, LV_STATE_DISABLED);
		lv_obj_set_clickable(replay_btn, true);
		lv_obj_set_style_opa(replay_btn, LV_OPA_COVER, 0);
	} else {
		lv_obj_add_state(replay_btn, LV_STATE_DISABLED);
		lv_obj_set_clickable(replay_btn, false);
		lv_obj_set_style_opa(replay_btn, LV_OPA_TRANSP, 0);
	}
}

static void reset_choice_styles(void) {
	for (int i = 0; i < CHOICES; i++) {
		style_choice(choice_btn[i], false, false);
		if (choice_lbl[i]) {
			lv_obj_set_style_text_color(choice_lbl[i], theme()->text_primary, 0);
		}
	}
}

static void stop_answer_feedback(void) {
	feedback_toggles_left = 0;
	feedback_holding = false;
	feedback_ok_idx = -1;
	if (feedback_timer) {
		lv_timer_pause(feedback_timer);
	}
}

static void advance_after_answer(void);

// Blink the correct row a few times, settle on the lit colour, then advance.
static void feedback_tick_cb(lv_timer_t *t) {
	(void)t;
	if (feedback_holding) {
		int ok = feedback_ok_idx;
		stop_answer_feedback();
		if (ok >= 0 && ok < CHOICES) {
			style_choice_ok(choice_btn[ok], ok, true);
		}
		advance_after_answer();
		return;
	}
	if (feedback_ok_idx < 0 || feedback_ok_idx >= CHOICES) {
		stop_answer_feedback();
		advance_after_answer();
		return;
	}
	feedback_blink_lit = !feedback_blink_lit;
	style_choice_ok(choice_btn[feedback_ok_idx], feedback_ok_idx, feedback_blink_lit);
	if (feedback_toggles_left > 0) {
		feedback_toggles_left--;
	}
	if (feedback_toggles_left <= 0) {
		// Last toggle should leave it lit; force solid and hold before preview.
		feedback_blink_lit = true;
		style_choice_ok(choice_btn[feedback_ok_idx], feedback_ok_idx, true);
		feedback_holding = true;
		if (feedback_timer) {
			lv_timer_set_period(feedback_timer, FEEDBACK_SOLID_MS);
			lv_timer_reset(feedback_timer);
		}
	}
}

static void start_answer_feedback(bool ok, int which) {
	stop_answer_feedback();
	feedback_ok_idx = round_correct;
	feedback_blink_lit = true;
	feedback_holding = false;
	// Start already lit (= flash 1). Remaining flips: off, on, off, on → 3 flashes total.
	feedback_toggles_left = (FEEDBACK_FLASHES - 1) * 2;
	if (feedback_toggles_left < 0) {
		feedback_toggles_left = 0;
	}

	for (int i = 0; i < CHOICES; i++) {
		lv_obj_add_state(choice_btn[i], LV_STATE_DISABLED);
		if (i == round_correct) {
			style_choice_ok(choice_btn[i], i, true);
		} else if (!ok && which == i) {
			// Final miss / last life: coral. Mid-run rejects stay grey above.
			style_choice_miss(choice_btn[i], i);
		} else if (choice_missed[i]) {
			style_choice_rejected(choice_btn[i], i);
		} else {
			style_choice(choice_btn[i], false, true);
		}
	}

	if (!feedback_timer) {
		advance_after_answer();
		return;
	}
	if (feedback_toggles_left == 0) {
		feedback_holding = true;
		lv_timer_set_period(feedback_timer, FEEDBACK_SOLID_MS);
	} else {
		lv_timer_set_period(feedback_timer, FEEDBACK_HALF_MS);
	}
	lv_timer_reset(feedback_timer);
	lv_timer_resume(feedback_timer);
}

static void show_idle(void);
static void hide_idle(void);

static void set_loading(bool on) {
	hide_reveal();
	if (load_spinner) {
		lv_obj_set_hidden(load_spinner, !on);
	}
	if (status_label) {
		if (on) {
			lv_label_set_text(status_label, ru() ? "Загрузка…" : "Loading…");
		}
		lv_obj_set_hidden(status_label, !on);
	}
	for (int i = 0; i < CHOICES; i++) {
		if (choice_btn[i]) {
			lv_obj_set_hidden(choice_btn[i], on);
		}
	}
	if (on || answered || round_correct < 0) {
		replay_set_available(false);
	} else {
		replay_set_available(true);
	}
	if (start_btn || idle_box) {
		hide_idle();
	}
	if (feedback_label) {
		lv_obj_set_hidden(feedback_label, true);
	}
	if (on) {
		stop_answer_timer();
	}
}

typedef struct {
	char path[PATH_MAX_QUIZ];
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

static void shuffle_ints(int *a, int n) {
	for (int i = n - 1; i > 0; i--) {
		int j = rand() % (i + 1);
		int tmp = a[i];
		a[i] = a[j];
		a[j] = tmp;
	}
}

static void fill_album(quiz_track_t *t) {
	t->album[0] = '\0';
	if (!t->path[0]) {
		return;
	}
	char value[256];
	if (library_track_album_value(t->path, value, sizeof(value)) && value[0]) {
		library_album_title(value, t->album, sizeof(t->album));
		if (!t->album[0]) {
			snprintf(t->album, sizeof(t->album), "%s", value);
		}
		return;
	}
	library_track_album(t->path, t->album, sizeof(t->album));
}

static void invalidate_pool(void) {
	pool_n = 0;
	pool_ready = false;
	memset(pool_used, 0, sizeof(pool_used));
	memset(pool, 0, sizeof(pool));
}

static bool cfg_same(const quiz_cfg_t *a, const quiz_cfg_t *b) {
	return a->ask == b->ask && a->play == b->play && a->reveal_on == b->reveal_on && a->reveal_sec == b->reveal_sec &&
		   a->survival_lives == b->survival_lives && a->timed_run_sec == b->timed_run_sec &&
		   a->timed_limit_sec == b->timed_limit_sec && a->timed_lives == b->timed_lives;
}

// Drop an in-progress game without requiring the Start screen to be visible yet
// (used when leaving the addon for More — loaded_cb will show_idle()).
static void abandon_session(void) {
	stop_answer_feedback();
	job_gen++; // drop any in-flight round worker
	busy = false;
	game_started = false;
	answered = false;
	revealing = false;
	round_correct = -1;
	run_active = false;
	run_needs_again = false;
	run_end_failed = false;
	lives_left = 0;
	score = 0;
	streak = 0;
	best_streak = 0;
	run_correct = 0;
	run_active = false;
	memset(choice_missed, 0, sizeof(choice_missed));
	last_result_line[0] = '\0';
	invalidate_pool();
	hide_reveal();
	hide_results();
}

static int pool_unused_count(void) {
	int n = 0;
	for (int i = 0; i < pool_n; i++) {
		if (!pool_used[i]) {
			n++;
		}
	}
	return n;
}

// force: rebuild even if a pool is already warm (new game / exhausted sample).
static bool ensure_pool(quiz_job_t *job, bool force) {
	if (!force && pool_ready && pool_n >= CHOICES) {
		return true;
	}
	pool_n = 0;
	pool_ready = false;
	memset(pool_used, 0, sizeof(pool_used));
	pool_failed_library = false;
	pool_failed_tracks = false;

	if (!library_is_open()) {
		job->no_library = true;
		pool_failed_library = true;
		return false;
	}

	library_index_t *ix = library_index_open(LIBRARY_LIST_TRACKS, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT,
											 false);
	int count = library_index_count(ix);
	if (count < CHOICES) {
		library_index_close(ix);
		job->no_tracks = true;
		pool_failed_tracks = true;
		return false;
	}

	int want = count < POOL_MAX ? count : POOL_MAX;
	for (int i = 0; i < want * 2 && pool_n < want; i++) {
		int idx = (int)(((uint32_t)rand() + (uint32_t)i * 2654435761u) % (uint32_t)count);
		row_catch_t c = {0};
		library_index_window(ix, idx, 1, catch_row, &c);
		if (!c.filled) {
			continue;
		}
		bool dup = false;
		for (int j = 0; j < pool_n; j++) {
			if (strcmp(pool[j].path, c.path) == 0) {
				dup = true;
				break;
			}
		}
		if (dup) {
			continue;
		}
		quiz_track_t *t = &pool[pool_n];
		snprintf(t->path, sizeof(t->path), "%s", c.path);
		snprintf(t->name, sizeof(t->name), "%s", c.name);
		snprintf(t->artist, sizeof(t->artist), "%s", c.artist);
		fill_album(t);
		pool_n++;
	}
	library_index_close(ix);

	if (pool_n < CHOICES) {
		job->no_tracks = true;
		pool_failed_tracks = true;
		return false;
	}
	pool_ready = true;
	return true;
}

static bool pick_round_from_pool(quiz_job_t *job) {
	if (!ensure_pool(job, false)) {
		return false;
	}
	// Every correct answer in this game must be a fresh pool track; when the
	// sample is spent, draw a new one rather than repeating.
	if (pool_unused_count() < 1) {
		if (!ensure_pool(job, true)) {
			return false;
		}
	}

	quiz_mode_t mode = job->mode;
	quiz_track_t picked[CHOICES];
	memset(picked, 0, sizeof(picked));

	int unused = pool_unused_count();
	int pick = rand() % unused;
	int correct_i = -1;
	for (int i = 0; i < pool_n; i++) {
		if (pool_used[i]) {
			continue;
		}
		if (pick-- == 0) {
			correct_i = i;
			break;
		}
	}
	if (correct_i < 0) {
		job->no_tracks = true;
		return false;
	}

	picked[0] = pool[correct_i];
	int have = 1;

	for (int pass = 0; pass < 2 && have < CHOICES; pass++) {
		for (int t = 0; t < pool_n * 2 && have < CHOICES; t++) {
			int idx = rand() % pool_n;
			if (idx == correct_i) {
				continue;
			}
			bool dup_path = false;
			for (int i = 0; i < have; i++) {
				if (strcmp(picked[i].path, pool[idx].path) == 0) {
					dup_path = true;
					break;
				}
			}
			if (dup_path) {
				continue;
			}
			if (pass == 0 && label_used(picked, have, mode, &pool[idx])) {
				continue;
			}
			picked[have++] = pool[idx];
		}
	}

	if (have < CHOICES) {
		job->no_tracks = true;
		return false;
	}

	pool_used[correct_i] = true;

	int order[CHOICES] = {0, 1, 2, 3};
	shuffle_ints(order, CHOICES);
	for (int i = 0; i < CHOICES; i++) {
		job->tracks[i] = picked[order[i]];
		if (order[i] == 0) {
			job->correct = i;
		}
	}
	return true;
}

static void finish_answer(bool ok, int which);
static void start_round(void);
static void end_run(bool failed);

static void answer_tick_cb(lv_timer_t *t) {
	(void)t;
	if (answered || busy || revealing || round_correct < 0) {
		stop_answer_timer();
		return;
	}
	uint32_t now = lv_tick_get();
	if (play_has_session_clock() && run_active) {
		int left = (int)run_deadline_ms - (int)now;
		if (left < 0) {
			left = 0;
		}
		run_left_ms = left;
		if (left == 0) {
			finish_answer(false, -1);
			return;
		}
	}
	if (per_answer_timer_active()) {
		int left = (int)answer_deadline_ms - (int)now;
		if (left < 0) {
			left = 0;
		}
		answer_left_ms = left;
		if (left == 0) {
			finish_answer(false, -1);
			return;
		}
	}
	paint_timer();
}

static void start_timers_for_round(void) {
	uint32_t now = lv_tick_get();

	// Open a scored session once at Start for Survival / Timed.
	if (play_has_lives() && !run_active) {
		run_active = true;
		score = 0;
		run_correct = 0;
		run_end_failed = false;
		lives_left = cfg_lives();
		if (play_has_session_clock()) {
			run_deadline_ms = now + (uint32_t)cfg.timed_run_sec * 1000u;
			run_left_ms = cfg.timed_run_sec * 1000;
		} else {
			run_left_ms = 0;
		}
		paint_score();
	} else if (cfg.play == PLAY_ENDLESS && !game_started) {
		// game_started is already true in start_cb; streak reset happens there.
	}

	if (play_has_session_clock() && run_active) {
		run_left_ms = (int)run_deadline_ms - (int)now;
		if (run_left_ms < 0) {
			run_left_ms = 0;
		}
	} else if (!play_has_session_clock()) {
		run_left_ms = 0;
	}

	if (per_answer_timer_active()) {
		answer_deadline_ms = now + (uint32_t)per_answer_ms();
		answer_left_ms = per_answer_ms();
	} else {
		answer_left_ms = 0;
	}
	paint_timer();
	if (answer_timer && (play_has_session_clock() || per_answer_timer_active())) {
		lv_timer_reset(answer_timer);
		lv_timer_resume(answer_timer);
	} else {
		stop_answer_timer();
	}
}

static void poll_reveal_cover(void) {
	if (!reveal_cover_pending || !reveal_cover_img) {
		return;
	}
	cover_image_t image;
	bool finished = false;
	bool found = coverloader_take(QUIZ_COVER_SLOT, &image, &finished);
	if (!finished) {
		return;
	}
	reveal_cover_pending = false;
	if (!found) {
		lv_obj_set_hidden(reveal_cover_img, true);
		return;
	}
	if (reveal_has_cover) {
		cover_free(&reveal_cover);
	}
	reveal_cover = image;
	reveal_has_cover = true;
	lv_image_set_src(reveal_cover_img, &reveal_cover.dsc);
	lv_obj_set_hidden(reveal_cover_img, false);
}

static void continue_after_answer(void);

static void reveal_done_cb(lv_timer_t *t) {
	(void)t;
	continue_after_answer();
}

static void reveal_tap_cb(lv_event_t *e) {
	(void)e;
	if (!revealing || switcher_back_drag_active()) {
		return;
	}
	continue_after_answer();
}

static void show_results(void) {
	hide_reveal();
	if (!results_box) {
		return;
	}
	if (cfg.play == PLAY_SURVIVAL) {
		records_note_survival(score);
	} else if (cfg.play == PLAY_TIMED) {
		records_note_timed(score);
	}

	if (results_title) {
		if (run_end_failed) {
			lv_label_set_text(results_title, ru() ? "Жизни закончились" : "Out of lives");
		} else {
			lv_label_set_text(results_title, ru() ? "Время вышло" : "Time's up");
		}
	}
	if (results_score) {
		char line[32];
		snprintf(line, sizeof(line), "%d", score);
		lv_label_set_text(results_score, line);
	}
	if (results_score_cap) {
		lv_label_set_text(results_score_cap, ru() ? "очков" : "points");
	}
	if (results_stat_correct_val) {
		char line[24];
		snprintf(line, sizeof(line), "%d", run_correct);
		lv_label_set_text(results_stat_correct_val, line);
	}
	if (results_stat_correct_lbl) {
		lv_label_set_text(results_stat_correct_lbl, ru() ? "угадано" : "correct");
	}
	if (results_stat_lives) {
		int lives_total = cfg_lives();
		bool show_lives = play_has_lives() && lives_total > 1;
		lv_obj_set_hidden(results_stat_lives, !show_lives);
		if (show_lives && results_stat_lives_val) {
			char line[24];
			snprintf(line, sizeof(line), "%d/%d", lives_left > 0 ? lives_left : 0, lives_total);
			lv_label_set_text(results_stat_lives_val, line);
		}
		if (show_lives && results_stat_lives_lbl) {
			lv_label_set_text(results_stat_lives_lbl, ru() ? "жизни" : "lives");
		}
	}
	if (results_btn_lbl) {
		lv_label_set_text(results_btn_lbl, ru() ? "Ещё раз" : "Play again");
	}

	lv_obj_set_hidden(results_box, false);
	lv_obj_move_foreground(results_box);
}

static void continue_after_answer(void) {
	// Timer and tap can both fire; a round already loading must not start twice.
	if (busy) {
		hide_reveal();
		return;
	}
	hide_reveal();
	if (run_needs_again) {
		show_results();
		return;
	}
	start_round();
}

static void show_preview(void) {
	if (!reveal_box || round_correct < 0) {
		continue_after_answer();
		return;
	}
	revealing = true;
	const quiz_track_t *t = &round_tracks[round_correct];
	if (reveal_result) {
		lv_label_set_text(reveal_result, last_result_line[0] ? last_result_line : "");
	}
	lv_label_set_text(reveal_title, t->name[0] ? t->name : t->path);
	char meta[512];
	snprintf(meta, sizeof(meta), "%s%s%s", t->artist, (t->artist[0] && t->album[0]) ? " · " : "", t->album);
	lv_label_set_text(reveal_meta, meta);

	if (reveal_cover_img) {
		lv_obj_set_size(reveal_cover_img, reveal_cover_px, reveal_cover_px);
	}
	free_reveal_cover();
	if (t->path[0]) {
		coverloader_start();
		coverloader_request(QUIZ_COVER_SLOT, t->path, reveal_cover_px);
		reveal_cover_pending = true;
		if (reveal_cover_img) {
			lv_obj_set_hidden(reveal_cover_img, true);
		}
	}

	lv_obj_set_hidden(reveal_box, false);
	lv_obj_move_foreground(reveal_box);

	int ms = cfg.reveal_sec * 1000;
	if (run_active && !run_needs_again && ms > 1500) {
		ms = 1500;
	}
	schedule_continue(ms);
}

// After an answer: track preview (or a short pause) → next round; run end → results.
static void advance_after_answer(void) {
	if (cfg.reveal_on) {
		show_preview();
		return;
	}
	if (run_needs_again) {
		show_results();
		return;
	}
	schedule_continue(AUTO_NEXT_MS);
}

static void end_run(bool failed) {
	run_active = false;
	run_needs_again = true;
	run_end_failed = failed;
	stop_answer_timer();
	replay_set_available(false);
	paint_score();
}

static void apply_round(void *user) {
	quiz_job_t *job = user;
	busy = false;
	if (!job) {
		return;
	}
	if (job->gen != job_gen || !quiz_screen) {
		free(job);
		return;
	}

	set_loading(false);

	if (!job->ok) {
		const char *msg;
		if (job->no_library || pool_failed_library) {
			msg = ru() ? "Сначала просканируйте\nбиблиотеку в Music." : "Scan the library\nunder Music first.";
		} else {
			msg = ru() ? "Нужно хотя бы 4 трека\nв библиотеке." : "Need at least 4 tracks\nin the library.";
		}
		game_started = false;
		show_idle();
		lv_label_set_text(status_label, msg);
		lv_obj_set_hidden(status_label, false);
		free(job);
		return;
	}

	answered = false;
	revealing = false;
	stop_answer_feedback();
	memset(choice_missed, 0, sizeof(choice_missed));
	round_correct = job->correct;
	memcpy(round_tracks, job->tracks, sizeof(round_tracks));

	hide_idle();
	lv_label_set_text(prompt_label, mode_prompt(cfg.ask));
	lv_obj_set_hidden(prompt_label, false);
	paint_score();
	reset_choice_styles();

	for (int i = 0; i < CHOICES; i++) {
		lv_label_set_text(choice_lbl[i], track_label(&round_tracks[i], cfg.ask));
		lv_obj_set_hidden(choice_btn[i], false);
		lv_obj_remove_state(choice_btn[i], LV_STATE_DISABLED);
	}
	lv_obj_set_hidden(feedback_label, true);
	lv_obj_set_hidden(status_label, true);
	replay_set_available(true);

	start_snippet(round_tracks[round_correct].path);
	start_timers_for_round();
	free(job);
}

static void *round_worker(void *arg) {
	quiz_job_t *job = arg;
	thread_be_background("quiz");
	job->ok = pick_round_from_pool(job);
	if (!gui_post(apply_round, job)) {
		free(job);
		busy = false;
	}
	return NULL;
}

static void start_round(void) {
	if (busy) {
		return;
	}
	stop_snippet();
	stop_answer_timer();
	hide_reveal();
	run_needs_again = false;
	hide_results();
	quiz_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->gen = ++job_gen;
	job->mode = cfg.ask;
	busy = true;
	answered = false;
	set_loading(true);

	pthread_t th;
	if (pthread_create(&th, NULL, round_worker, job) != 0) {
		busy = false;
		free(job);
		set_loading(false);
		lv_label_set_text(status_label, ru() ? "Ошибка потока." : "Thread error.");
		lv_obj_set_hidden(status_label, false);
		return;
	}
	pthread_detach(th);
}

static void finish_answer(bool ok, int which) {
	if (answered || round_correct < 0 || revealing) {
		return;
	}
	answered = true;

	// Snapshot timers before stop_answer_timer() clears the per-answer left ms.
	int answer_left_snap = answer_left_ms;
	bool session_expired = false;
	if (play_has_session_clock() && run_active) {
		int left = (int)run_deadline_ms - (int)lv_tick_get();
		if (left < 0) {
			left = 0;
		}
		run_left_ms = left;
		session_expired = left == 0;
	}

	stop_snippet();
	stop_answer_timer();

	int bonus = 0;
	if (ok) {
		bonus = 1;
		if (per_answer_timer_active() && answer_left_snap > 0) {
			bonus += answer_left_snap / 3000;
		}
		if (cfg.play == PLAY_ENDLESS) {
			streak++;
			if (streak > best_streak) {
				best_streak = streak;
			}
			records_note_endless(streak);
		} else {
			score += bonus;
			if (run_active) {
				run_correct++;
			}
		}
	} else {
		if (cfg.play == PLAY_ENDLESS) {
			streak = 0;
		} else if (run_active && play_has_lives() && lives_left > 0 && which >= 0) {
			// Last life spent on this miss (timeout path uses which < 0).
			lives_left = 0;
		}
	}
	paint_score();

	char line[320];
	const char *right = track_label(&round_tracks[round_correct], cfg.ask);
	if (ok) {
		if (cfg.play == PLAY_ENDLESS) {
			snprintf(line, sizeof(line), ru() ? "Верно! серия %d" : "Correct! streak %d", streak);
		} else {
			snprintf(line, sizeof(line), ru() ? "Верно! +%d" : "Correct! +%d", bonus);
		}
	} else if (which < 0) {
		snprintf(line, sizeof(line), ru() ? "Время! — %s" : "Time! — %s", right);
	} else if (cfg.play == PLAY_ENDLESS) {
		snprintf(line, sizeof(line), ru() ? "Мимо — серия сброшена" : "Miss — streak reset");
	} else {
		snprintf(line, sizeof(line), ru() ? "Нет — %s" : "No — %s", right);
	}
	snprintf(last_result_line, sizeof(last_result_line), "%s", line);
	lv_label_set_text(feedback_label, line);
	lv_obj_set_hidden(feedback_label, cfg.reveal_on);
	replay_set_available(false);

	// Survival/Timed: end when lives are gone, or Timed session clock hits zero.
	// Endless never ends a session here.
	bool end_session = false;
	if (cfg.play != PLAY_ENDLESS && run_active) {
		if (session_expired) {
			end_session = true;
			run_end_failed = false;
		} else if (!ok) {
			end_session = true;
			run_end_failed = true;
		}
	}
	if (end_session) {
		end_run(run_end_failed);
	}

	start_answer_feedback(ok, which);
}

// Wrong pick with lives left: spend one and keep the round open.
static bool try_spend_life(int which) {
	if (!play_has_lives() || !run_active || lives_left <= 1 || which < 0 || which >= CHOICES) {
		return false;
	}
	if (choice_missed[which]) {
		return true; // already spent on this button
	}
	choice_missed[which] = true;
	lives_left--;
	paint_lives();
	lv_obj_add_state(choice_btn[which], LV_STATE_DISABLED);
	style_choice_rejected(choice_btn[which], which);
	lv_label_set_text(feedback_label, ru() ? "Мимо — ещё попытка" : "Miss — try again");
	lv_obj_set_hidden(feedback_label, false);
	return true;
}

static void choice_cb(lv_event_t *e) {
	if (answered || busy || revealing || !game_started || switcher_back_drag_active()) {
		return;
	}
	int which = (int)(intptr_t)lv_event_get_user_data(e);
	if (which < 0 || which >= CHOICES || round_correct < 0) {
		return;
	}
	if (choice_missed[which]) {
		return;
	}
	if (which == round_correct) {
		finish_answer(true, which);
		return;
	}
	if (try_spend_life(which)) {
		return;
	}
	finish_answer(false, which);
}

static void results_restart_cb(lv_event_t *e) {
	(void)e;
	if (busy || switcher_back_drag_active()) {
		return;
	}
	run_needs_again = false;
	run_active = false; // start_timers_for_round will open a fresh run if enabled
	hide_results();
	game_started = true;
	invalidate_pool(); // new game → fresh library sample
	start_round();
}

static void show_idle(void) {
	game_started = false;
	answered = false;
	revealing = false;
	round_correct = -1;
	run_active = false;
	run_needs_again = false;
	lives_left = 0;
	memset(choice_missed, 0, sizeof(choice_missed));
	stop_snippet();
	stop_answer_timer();
	hide_reveal();
	hide_results();
	if (load_spinner) {
		lv_obj_set_hidden(load_spinner, true);
	}
	for (int i = 0; i < CHOICES; i++) {
		if (choice_btn[i]) {
			lv_obj_set_hidden(choice_btn[i], true);
		}
	}
	if (replay_btn) {
		lv_obj_set_hidden(replay_btn, true);
	}
	if (feedback_label) {
		lv_obj_set_hidden(feedback_label, true);
	}
	if (status_label) {
		lv_obj_set_hidden(status_label, true);
	}
	if (timer_label) {
		lv_label_set_text(timer_label, "");
	}
	if (prompt_label) {
		lv_obj_set_hidden(prompt_label, true);
	}
	if (meta_row) {
		lv_obj_set_hidden(meta_row, true);
	}
	if (idle_box) {
		lv_obj_set_hidden(idle_box, false);
	}
	paint_score();
}

static void hide_idle(void) {
	if (idle_box) {
		lv_obj_set_hidden(idle_box, true);
	}
	if (meta_row) {
		lv_obj_set_hidden(meta_row, false);
	}
}

// Compact start-menu pills: equal-width, one row, no nested settings cards
// (those cards overflow the 320x480 page and force a scrollbar).
static lv_obj_t *idle_section(lv_obj_t *parent, const char *title, lv_obj_t **cap_out, lv_obj_t **row_out) {
	lv_obj_t *sec = lv_obj_create(parent);
	lv_obj_remove_style_all(sec);
	lv_obj_set_width(sec, lv_pct(100));
	lv_obj_set_height(sec, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(sec, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(sec, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(sec, SC(8), 0);
	lv_obj_set_scrollable(sec, false);

	lv_obj_t *cap = lv_label_create(sec);
	lv_obj_set_style_text_font(cap, &font_ui_18, 0);
	lv_obj_set_style_text_color(cap, theme()->text_secondary, 0);
	lv_label_set_text(cap, title);

	lv_obj_t *row = lv_obj_create(sec);
	lv_obj_remove_style_all(row);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, SC(8), 0);
	lv_obj_set_scrollable(row, false);

	*cap_out = cap;
	*row_out = row;
	return sec;
}

static lv_obj_t *idle_pill(lv_obj_t *parent, const char *text, int value, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_height(btn, SC(48));
	lv_obj_set_flex_grow(btn, 1);
	lv_obj_set_style_pad_hor(btn, SC(4), 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)value);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, &font_ui_18, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_center(label);
	return btn;
}

static void start_cb(lv_event_t *e) {
	(void)e;
	if (busy || switcher_back_drag_active()) {
		return;
	}
	game_started = true;
	run_active = false; // start_timers_for_round opens Survival/Timed session
	score = 0;
	streak = 0;
	best_streak = 0;
	run_correct = 0;
	hide_idle();
	invalidate_pool();
	start_round();
}

static void replay_cb(lv_event_t *e) {
	(void)e;
	if (answered || busy || revealing || round_correct < 0 || switcher_back_drag_active()) {
		return;
	}
	start_snippet(round_tracks[round_correct].path);
}

static void refresh_ask_pills(void) {
	for (int i = 0; i < MODE_COUNT; i++) {
		if (set_ask_pills[i]) {
			settingsrow_pill_active(set_ask_pills[i], i == (int)cfg.ask);
		}
		if (idle_ask_pills[i]) {
			settingsrow_pill_active(idle_ask_pills[i], i == (int)cfg.ask);
		}
	}
}

static void refresh_play_pills(void) {
	for (int i = 0; i < PLAY_COUNT; i++) {
		if (set_play_pills[i]) {
			settingsrow_pill_active(set_play_pills[i], i == (int)cfg.play);
		}
		if (idle_play_pills[i]) {
			settingsrow_pill_active(idle_play_pills[i], i == (int)cfg.play);
		}
	}
	if (set_survival_box) {
		lv_obj_set_hidden(set_survival_box, cfg.play != PLAY_SURVIVAL);
	}
	if (set_timed_box) {
		lv_obj_set_hidden(set_timed_box, cfg.play != PLAY_TIMED);
	}
}

static void paint_idle_blurb(void) {
	if (!idle_mode_blurb) {
		return;
	}
	char line[192];
	if (cfg.play == PLAY_SURVIVAL) {
		snprintf(line, sizeof(line),
				 ru() ? "Жизни: %d. Ошибки тратят жизни.\nСчёт растёт за верные ответы."
					  : "Lives: %d. Misses cost a life.\nScore grows with correct answers.",
				 cfg.survival_lives);
	} else if (cfg.play == PLAY_ENDLESS) {
		snprintf(line, sizeof(line),
				 ru() ? "Без конца и жизней.\nСерия растёт за верные ответы,\nсбрасывается при ошибке."
					  : "No end, no lives.\nStreak grows on hits,\nresets on a miss.");
	} else {
		snprintf(line, sizeof(line),
				 ru() ? "Сессия %d с, ответ %d с, жизни: %d.\nНаберите очки до конца времени."
					  : "Session %d s, answer %d s, lives: %d.\nScore before the clock runs out.",
				 cfg.timed_run_sec, cfg.timed_limit_sec, cfg.timed_lives);
	}
	lv_label_set_text(idle_mode_blurb, line);
}

static void sync_cfg_ui(void) {
	refresh_ask_pills();
	refresh_play_pills();
	paint_idle_blurb();
}

static void ask_pill_cb(lv_event_t *e) {
	int v = (int)(intptr_t)lv_event_get_user_data(e);
	if (v < 0 || v >= MODE_COUNT) {
		return;
	}
	cfg.ask = (quiz_mode_t)v;
	cfg_save();
	sync_cfg_ui();
}

static void play_pill_cb(lv_event_t *e) {
	int v = (int)(intptr_t)lv_event_get_user_data(e);
	if (v < 0 || v >= PLAY_COUNT) {
		return;
	}
	cfg.play = (play_mode_t)v;
	cfg_save();
	sync_cfg_ui();
}

static void set_sec_label(lv_obj_t *lab, int sec) {
	char line[32];
	snprintf(line, sizeof(line), ru() ? "%d с" : "%d s", sec);
	lv_label_set_text(lab, line);
}

static void set_lives_label(lv_obj_t *lab, int n) {
	char line[16];
	snprintf(line, sizeof(line), "%d", n);
	lv_label_set_text(lab, line);
}

static void survival_lives_cb(lv_event_t *e) {
	int idx = (int)lv_slider_get_value(lv_event_get_target(e));
	if (idx < 0) {
		idx = 0;
	}
	if (idx >= LIVES_N) {
		idx = LIVES_N - 1;
	}
	cfg.survival_lives = LIVES_N_OPTS[idx];
	set_lives_label(set_survival_lives_val, cfg.survival_lives);
	cfg_save();
	paint_idle_blurb();
}

static void timed_run_cb(lv_event_t *e) {
	int idx = (int)lv_slider_get_value(lv_event_get_target(e));
	if (idx < 0) {
		idx = 0;
	}
	if (idx >= RUN_N) {
		idx = RUN_N - 1;
	}
	cfg.timed_run_sec = RUN_SECS[idx];
	set_sec_label(set_timed_run_val, cfg.timed_run_sec);
	cfg_save();
	paint_idle_blurb();
}

static void timed_limit_cb(lv_event_t *e) {
	int idx = (int)lv_slider_get_value(lv_event_get_target(e));
	if (idx < 0) {
		idx = 0;
	}
	if (idx >= LIMIT_N) {
		idx = LIMIT_N - 1;
	}
	cfg.timed_limit_sec = LIMIT_SECS[idx];
	set_sec_label(set_timed_limit_val, cfg.timed_limit_sec);
	cfg_save();
	paint_idle_blurb();
}

static void timed_lives_cb(lv_event_t *e) {
	int idx = (int)lv_slider_get_value(lv_event_get_target(e));
	if (idx < 0) {
		idx = 0;
	}
	if (idx >= LIVES_N) {
		idx = LIVES_N - 1;
	}
	cfg.timed_lives = LIVES_N_OPTS[idx];
	set_lives_label(set_timed_lives_val, cfg.timed_lives);
	cfg_save();
	paint_idle_blurb();
}

static void reveal_toggle_cb(lv_event_t *e) {
	cfg.reveal_on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	settingsrow_toggle_slider_expanded(reveal_card, cfg.reveal_on);
	cfg_save();
}

static void reveal_slider_cb(lv_event_t *e) {
	int idx = (int)lv_slider_get_value(lv_event_get_target(e));
	if (idx < 0) {
		idx = 0;
	}
	if (idx >= REVEAL_N) {
		idx = REVEAL_N - 1;
	}
	cfg.reveal_sec = REVEAL_SECS[idx];
	set_sec_label(reveal_val, cfg.reveal_sec);
	cfg_save();
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	if (!settings_screen) {
		return;
	}
	cfg_before_settings = cfg;
	switch_screen(settings_screen);
}

static void paint_records(void) {
	char line[48];
	if (records_surv_val) {
		if (best_survival_score > 0) {
			snprintf(line, sizeof(line), ru() ? "%d очк." : "%d pts", best_survival_score);
		} else {
			snprintf(line, sizeof(line), "—");
		}
		lv_label_set_text(records_surv_val, line);
	}
	if (records_end_val) {
		if (best_endless_streak > 0) {
			snprintf(line, sizeof(line), ru() ? "серия %d" : "streak %d", best_endless_streak);
		} else {
			snprintf(line, sizeof(line), "—");
		}
		lv_label_set_text(records_end_val, line);
	}
	if (records_timed_val) {
		if (best_timed_score > 0) {
			snprintf(line, sizeof(line), ru() ? "%d очк." : "%d pts", best_timed_score);
		} else {
			snprintf(line, sizeof(line), "—");
		}
		lv_label_set_text(records_timed_val, line);
	}
}

static void open_records_cb(lv_event_t *e) {
	(void)e;
	if (!records_screen) {
		return;
	}
	paint_records();
	switch_screen(records_screen);
}

static void records_clear_cb(lv_event_t *e) {
	(void)e;
	records_clear();
	paint_records();
}

static void build_records(void) {
	records_screen = lv_obj_create(NULL);
	sonix_addon_own_screen(addon_id, records_screen);
	lv_obj_t *page = settingsrow_page(records_screen, gui_cfg, ru() ? "Рекорды" : "Records");

	settingsrow_add(page, ru() ? "Выживание" : "Survival", &records_surv_val, NULL, NULL);
	settingsrow_add(page, ru() ? "Бесконечный" : "Endless", &records_end_val, NULL, NULL);
	settingsrow_add(page, ru() ? "На время" : "Timed", &records_timed_val, NULL, NULL);
	settingsrow_action(page, ru() ? "Сбросить рекорды" : "Clear records", records_clear_cb, NULL);
	paint_records();
	switcher_attach_back_gesture(records_screen);
}

static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
							   lv_event_cb_t cb, lv_obj_t **icon_out) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, SC(56), SC(56));
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (SC(56) + SC(6)),
				 cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	if (icon_out) {
		*icon_out = icon;
	}
	return button;
}

static lv_obj_t *make_settings_section(lv_obj_t *page) {
	lv_obj_t *box = lv_obj_create(page);
	lv_obj_remove_style_all(box);
	lv_obj_set_width(box, lv_pct(100));
	lv_obj_set_height(box, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(box, SC(8), 0);
	return box;
}

static void build_settings(void) {
	settings_screen = lv_obj_create(NULL);
	sonix_addon_own_screen(addon_id, settings_screen);
	lv_obj_t *page = settingsrow_page(settings_screen, gui_cfg, ru() ? "Настройки" : "Settings");

	lv_obj_t *ask_row = NULL;
	settingsrow_pills(page, ru() ? "Что угадывать" : "What to guess", &ask_row);
	set_ask_pills[MODE_ARTIST] =
		settingsrow_pill_text(ask_row, ru() ? "Исполнитель" : "Artist", MODE_ARTIST, ask_pill_cb);
	set_ask_pills[MODE_TRACK] = settingsrow_pill_text(ask_row, ru() ? "Трек" : "Track", MODE_TRACK, ask_pill_cb);
	set_ask_pills[MODE_ALBUM] = settingsrow_pill_text(ask_row, ru() ? "Альбом" : "Album", MODE_ALBUM, ask_pill_cb);

	lv_obj_t *play_row = NULL;
	settingsrow_pills(page, ru() ? "Режим" : "Mode", &play_row);
	set_play_pills[PLAY_SURVIVAL] =
		settingsrow_pill_text(play_row, ru() ? "Выживание" : "Survival", PLAY_SURVIVAL, play_pill_cb);
	set_play_pills[PLAY_ENDLESS] =
		settingsrow_pill_text(play_row, ru() ? "Бесконечный" : "Endless", PLAY_ENDLESS, play_pill_cb);
	set_play_pills[PLAY_TIMED] = settingsrow_pill_text(play_row, ru() ? "На время" : "Timed", PLAY_TIMED, play_pill_cb);

	set_survival_box = make_settings_section(page);
	set_survival_lives_card = settingsrow_slider(set_survival_box, ru() ? "Жизни" : "Lives", LIVES_N,
												 &set_survival_lives_val, &set_survival_lives_slider, survival_lives_cb);
	lv_slider_set_value(set_survival_lives_slider, nearest_index(LIVES_N_OPTS, LIVES_N, cfg.survival_lives), LV_ANIM_OFF);
	set_lives_label(set_survival_lives_val, cfg.survival_lives);

	set_timed_box = make_settings_section(page);
	set_timed_run_card = settingsrow_slider(set_timed_box, ru() ? "Длина сессии" : "Session length", RUN_N,
											&set_timed_run_val, &set_timed_run_slider, timed_run_cb);
	lv_slider_set_value(set_timed_run_slider, nearest_index(RUN_SECS, RUN_N, cfg.timed_run_sec), LV_ANIM_OFF);
	set_sec_label(set_timed_run_val, cfg.timed_run_sec);

	set_timed_limit_card = settingsrow_slider(set_timed_box, ru() ? "Время на ответ" : "Time per answer", LIMIT_N,
											  &set_timed_limit_val, &set_timed_limit_slider, timed_limit_cb);
	lv_slider_set_value(set_timed_limit_slider, nearest_index(LIMIT_SECS, LIMIT_N, cfg.timed_limit_sec), LV_ANIM_OFF);
	set_sec_label(set_timed_limit_val, cfg.timed_limit_sec);

	set_timed_lives_card = settingsrow_slider(set_timed_box, ru() ? "Жизни" : "Lives", LIVES_N, &set_timed_lives_val,
											  &set_timed_lives_slider, timed_lives_cb);
	lv_slider_set_value(set_timed_lives_slider, nearest_index(LIVES_N_OPTS, LIVES_N, cfg.timed_lives), LV_ANIM_OFF);
	set_lives_label(set_timed_lives_val, cfg.timed_lives);

	reveal_card =
		settingsrow_toggle_slider(page, ru() ? "Показать трек" : "Show the track", REVEAL_N, &reveal_sw, &reveal_val,
								  &reveal_slider, reveal_toggle_cb, reveal_slider_cb);
	if (cfg.reveal_on) {
		lv_obj_add_state(reveal_sw, LV_STATE_CHECKED);
	}
	lv_slider_set_value(reveal_slider, nearest_index(REVEAL_SECS, REVEAL_N, cfg.reveal_sec), LV_ANIM_OFF);
	set_sec_label(reveal_val, cfg.reveal_sec);
	settingsrow_toggle_slider_expanded(reveal_card, cfg.reveal_on);

	refresh_ask_pills();
	refresh_play_pills();
	switcher_attach_back_gesture(settings_screen);
}

static void theme_cb(void) {
	if (prompt_label) {
		lv_obj_set_style_text_color(prompt_label, theme()->text_primary, 0);
	}
	if (score_label) {
		lv_obj_set_style_text_color(score_label, theme()->text_secondary, 0);
	}
	if (lives_row && hearts_ready) {
		int total = cfg_lives();
		if (total > LIVES_MAX) {
			total = LIVES_MAX;
		}
		for (int i = 0; i < total; i++) {
			heart_tint(lives_img[i], i < lives_left);
		}
	}
	if (timer_label) {
		lv_obj_set_style_text_color(timer_label, theme()->accent, 0);
	}
	if (idle_box) {
		lv_obj_set_style_bg_color(idle_box, theme()->surface, 0);
	}
	if (idle_title) {
		lv_obj_set_style_text_color(idle_title, theme()->text_primary, 0);
	}
	if (idle_ask_cap) {
		lv_obj_set_style_text_color(idle_ask_cap, theme()->text_secondary, 0);
	}
	if (idle_play_cap) {
		lv_obj_set_style_text_color(idle_play_cap, theme()->text_secondary, 0);
	}
	if (idle_mode_blurb) {
		lv_obj_set_style_text_color(idle_mode_blurb, theme()->text_secondary, 0);
	}
	if (start_btn) {
		lv_obj_set_style_bg_color(start_btn, theme()->accent, 0);
	}
	if (feedback_label) {
		lv_obj_set_style_text_color(feedback_label, theme()->text_primary, 0);
	}
	if (status_label) {
		lv_obj_set_style_text_color(status_label, theme()->text_secondary, 0);
	}
	if (replay_btn) {
		lv_obj_set_style_bg_color(replay_btn, theme()->surface, 0);
	}
	if (reveal_box) {
		lv_obj_set_style_bg_color(reveal_box, theme()->screen_bg, 0);
	}
	if (reveal_result) {
		lv_obj_set_style_text_color(reveal_result, theme()->accent, 0);
	}
	if (reveal_title) {
		lv_obj_set_style_text_color(reveal_title, theme()->text_primary, 0);
	}
	if (reveal_meta) {
		lv_obj_set_style_text_color(reveal_meta, theme()->text_secondary, 0);
	}
	if (results_box) {
		lv_obj_set_style_bg_color(results_box, theme()->screen_bg, 0);
	}
	if (results_card) {
		lv_obj_set_style_bg_color(results_card, theme()->surface, 0);
	}
	if (results_title) {
		lv_obj_set_style_text_color(results_title, theme()->text_secondary, 0);
	}
	if (results_score) {
		lv_obj_set_style_text_color(results_score, theme()->accent, 0);
	}
	if (results_score_cap) {
		lv_obj_set_style_text_color(results_score_cap, theme()->text_secondary, 0);
	}
	if (results_stat_correct_val) {
		lv_obj_set_style_text_color(results_stat_correct_val, theme()->text_primary, 0);
	}
	if (results_stat_correct_lbl) {
		lv_obj_set_style_text_color(results_stat_correct_lbl, theme()->text_secondary, 0);
	}
	if (results_stat_lives_val) {
		lv_obj_set_style_text_color(results_stat_lives_val, theme()->text_primary, 0);
	}
	if (results_stat_lives_lbl) {
		lv_obj_set_style_text_color(results_stat_lives_lbl, theme()->text_secondary, 0);
	}
	if (results_btn) {
		lv_obj_set_style_bg_color(results_btn, theme()->accent, 0);
	}
	if (!answered) {
		reset_choice_styles();
	}
	refresh_ask_pills();
	refresh_play_pills();
}

static void poll_cb(lv_timer_t *t) {
	(void)t;
	if (quiz_screen && lv_screen_active() == quiz_screen) {
		poll_reveal_cover();
	}
}

static lv_timer_t *poll_timer;

static void loaded_cb(lv_event_t *e) {
	(void)e;
	if (poll_timer) {
		lv_timer_resume(poll_timer);
	}
	// Settings changed while away → Start screen, new session.
	if (!cfg_same(&cfg, &cfg_before_settings)) {
		cfg_before_settings = cfg;
		abandon_session();
		show_idle();
		return;
	}
	// Resume only a live unanswered round. Otherwise Start (also covers a fresh
	// entry after leaving the addon for More).
	if (!game_started || answered || revealing || busy || round_correct < 0 ||
		!round_tracks[round_correct].path[0]) {
		if (game_started && (answered || revealing || busy)) {
			abandon_session();
		}
		show_idle();
		return;
	}
	start_snippet(round_tracks[round_correct].path);
	start_timers_for_round();
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	stop_snippet();
	stop_answer_timer();
	stop_answer_feedback();
	if (poll_timer) {
		lv_timer_pause(poll_timer);
	}
	// Leaving the addon (More, etc.) — not a detour into Settings/Records.
	// Next entry must open on Start, not resume a half-finished run.
	if (lv_screen_active() != settings_screen && lv_screen_active() != records_screen) {
		abandon_session();
	}
}

static lv_obj_t *make_tool_btn(lv_obj_t *parent, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_height(btn, TOOL_H);
	lv_obj_set_style_radius(btn, SC(12), 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_bg_color(btn, theme()->surface, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(btn, SC(12), 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	return btn;
}

static lv_obj_t *make_choice(lv_obj_t *parent, int index) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_width(btn, lv_pct(100));
	lv_obj_set_height(btn, CHOICE_H);
	lv_obj_set_style_radius(btn, SC(12), 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_pad_hor(btn, SC(16), 0);
	style_choice(btn, false, false);
	lv_obj_add_event_cb(btn, choice_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
	lv_obj_set_width(lbl, lv_pct(100));
	lv_obj_set_style_text_font(lbl, &font_ui_22, 0);
	lv_obj_set_style_text_color(lbl, theme()->text_primary, 0);
	lv_label_set_text(lbl, "");
	lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
	choice_lbl[index] = lbl;
	return btn;
}

static lv_obj_t *quiz_addon_init(const sonix_addon_host_t *host) {
	gui_cfg = host->cfg;
	addon_id = host->id;
	addon_dir = host->dir;
	store_open(host->dir);
	cfg_before_settings = cfg;
	srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)host);
	coverloader_start();

	quiz_screen = lv_obj_create(NULL);
	lv_obj_add_style(quiz_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(quiz_screen, false);
	sonix_addon_own_screen(addon_id, quiz_screen);

	lv_obj_t *title = settingsrow_title(quiz_screen, gui_cfg, ru() ? "Викторина" : "Quiz");
	settingsrow_title_corner_slots(title, gui_cfg, 2);

	corner_button(quiz_screen, gui_cfg, 0, &icon_music_settings, open_settings_cb, NULL);
	trophy_ready = heart_sprite_load("trophy.png", &trophy_dsc, &trophy_px);
	if (trophy_ready) {
		corner_button(quiz_screen, gui_cfg, 1, &trophy_dsc, open_records_cb, &trophy_icon);
	} else {
		corner_button(quiz_screen, gui_cfg, 1, &icon_star_corner, open_records_cb, &trophy_icon);
	}

	int top = settingsrow_content_top(gui_cfg);
	int pad = host->cfg->padding;
	int sw = (int)host->cfg->screen_width;

	lv_obj_t *page = lv_obj_create(quiz_screen);
	lv_obj_remove_style_all(page);
	lv_obj_set_pos(page, pad, top);
	lv_obj_set_size(page, sw - 2 * pad, (int)host->cfg->screen_height - top - pad);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(page, CHOICE_GAP, 0);
	lv_obj_set_scrollable(page, true);
	switcher_attach_back_gesture(page);

	prompt_label = lv_label_create(page);
	lv_obj_set_width(prompt_label, lv_pct(100));
	lv_obj_set_style_text_font(prompt_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(prompt_label, theme()->text_primary, 0);
	lv_obj_set_style_text_align(prompt_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(prompt_label, "");

	meta_row = lv_obj_create(page);
	lv_obj_remove_style_all(meta_row);
	lv_obj_set_width(meta_row, lv_pct(100));
	lv_obj_set_height(meta_row, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(meta_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(meta_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(meta_row, SC(12), 0);

	score_label = lv_label_create(meta_row);
	lv_obj_set_flex_grow(score_label, 1);
	lv_obj_set_width(score_label, 0); // share leftover with grow; truncate if long
	lv_label_set_long_mode(score_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_font(score_label, &font_ui_18, 0);
	lv_obj_set_style_text_color(score_label, theme()->text_secondary, 0);

	hearts_ready = heart_sprite_load("heart.png", &heart_full_dsc, &heart_full_px) &&
				   heart_sprite_load("heart_empty.png", &heart_empty_dsc, &heart_empty_px);

	// Fixed slot for LIVES_MAX hearts so the row never changes width mid-run
	// (and stays put while the timer digits tick).
	lives_row = lv_obj_create(meta_row);
	lv_obj_remove_style_all(lives_row);
	lv_obj_set_size(lives_row, LIVES_MAX * HEART_PX + (LIVES_MAX - 1) * HEART_GAP, HEART_PX);
	lv_obj_set_flex_flow(lives_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(lives_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(lives_row, HEART_GAP, 0);
	lv_obj_set_scrollable(lives_row, false);
	lv_obj_set_hidden(lives_row, true);
	for (int i = 0; i < LIVES_MAX; i++) {
		lv_obj_t *img = lv_image_create(lives_row);
		lv_obj_set_size(img, HEART_PX, HEART_PX);
		if (hearts_ready) {
			lv_image_set_src(img, &heart_full_dsc);
			lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
			heart_tint(img, true);
		}
		lv_obj_set_hidden(img, true);
		lives_img[i] = img;
	}

	timer_label = lv_label_create(meta_row);
	lv_obj_set_width(timer_label, TIMER_SLOT_W);
	lv_obj_set_style_text_align(timer_label, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_style_text_font(timer_label, &font_ui_26, 0);
	lv_obj_set_style_text_color(timer_label, theme()->accent, 0);
	lv_label_set_text(timer_label, "");

	paint_score();

	// Fills the page on Start; options stay at the top, Start sticks to the bottom.
	idle_box = lv_obj_create(page);
	lv_obj_remove_style_all(idle_box);
	lv_obj_set_width(idle_box, lv_pct(100));
	lv_obj_set_flex_grow(idle_box, 1);
	lv_obj_set_style_bg_color(idle_box, theme()->surface, 0);
	lv_obj_set_style_bg_opa(idle_box, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(idle_box, SC(16), 0);
	lv_obj_set_style_pad_hor(idle_box, SC(16), 0);
	lv_obj_set_style_pad_ver(idle_box, SC(16), 0);
	lv_obj_set_style_pad_row(idle_box, SC(10), 0);
	lv_obj_set_flex_flow(idle_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(idle_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(idle_box, false);

	idle_title = lv_label_create(idle_box);
	lv_obj_set_width(idle_title, lv_pct(100));
	lv_obj_set_style_text_align(idle_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(idle_title, &font_ui_24, 0);
	lv_obj_set_style_text_color(idle_title, theme()->text_primary, 0);
	lv_label_set_text(idle_title, ru() ? "Новая игра" : "New game");

	idle_section(idle_box, ru() ? "Угадывать" : "Guess", &idle_ask_cap, &idle_ask_row);
	idle_ask_pills[MODE_ARTIST] = idle_pill(idle_ask_row, ru() ? "Исполн." : "Artist", MODE_ARTIST, ask_pill_cb);
	idle_ask_pills[MODE_TRACK] = idle_pill(idle_ask_row, ru() ? "Трек" : "Track", MODE_TRACK, ask_pill_cb);
	idle_ask_pills[MODE_ALBUM] = idle_pill(idle_ask_row, ru() ? "Альбом" : "Album", MODE_ALBUM, ask_pill_cb);

	idle_section(idle_box, ru() ? "Режим" : "Mode", &idle_play_cap, &idle_play_row);
	idle_play_pills[PLAY_SURVIVAL] =
		idle_pill(idle_play_row, ru() ? "Выжив." : "Survival", PLAY_SURVIVAL, play_pill_cb);
	idle_play_pills[PLAY_ENDLESS] =
		idle_pill(idle_play_row, ru() ? "Бескон." : "Endless", PLAY_ENDLESS, play_pill_cb);
	idle_play_pills[PLAY_TIMED] = idle_pill(idle_play_row, ru() ? "Время" : "Timed", PLAY_TIMED, play_pill_cb);

	idle_mode_blurb = lv_label_create(idle_box);
	lv_label_set_long_mode(idle_mode_blurb, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(idle_mode_blurb, lv_pct(100));
	lv_obj_set_style_text_align(idle_mode_blurb, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(idle_mode_blurb, &font_ui_18, 0);
	lv_obj_set_style_text_color(idle_mode_blurb, theme()->text_secondary, 0);
	lv_obj_set_style_pad_top(idle_mode_blurb, SC(2), 0);
	lv_label_set_text(idle_mode_blurb, "");

	refresh_ask_pills();
	refresh_play_pills();
	paint_idle_blurb();

	lv_obj_t *idle_spacer = lv_obj_create(idle_box);
	lv_obj_remove_style_all(idle_spacer);
	lv_obj_set_width(idle_spacer, lv_pct(100));
	lv_obj_set_flex_grow(idle_spacer, 1);
	lv_obj_set_scrollable(idle_spacer, false);

	start_btn = lv_btn_create(idle_box);
	lv_obj_set_width(start_btn, lv_pct(100));
	lv_obj_set_height(start_btn, TOOL_H);
	lv_obj_set_style_radius(start_btn, SC(12), 0);
	lv_obj_set_style_shadow_width(start_btn, 0, 0);
	lv_obj_set_style_border_width(start_btn, 0, 0);
	lv_obj_set_style_bg_color(start_btn, theme()->accent, 0);
	lv_obj_set_style_bg_opa(start_btn, LV_OPA_COVER, 0);
	lv_obj_add_event_cb(start_btn, start_cb, LV_EVENT_CLICKED, NULL);

	start_lbl = lv_label_create(start_btn);
	lv_obj_set_style_text_font(start_lbl, &font_ui_22, 0);
	lv_obj_set_style_text_color(start_lbl, lv_color_white(), 0);
	lv_label_set_text(start_lbl, ru() ? "Начать" : "Start");
	lv_obj_center(start_lbl);

	replay_btn = make_tool_btn(page, replay_cb);
	lv_obj_set_width(replay_btn, lv_pct(100));
	replay_lbl = lv_label_create(replay_btn);
	lv_obj_set_style_text_font(replay_lbl, &font_ui_18, 0);
	lv_obj_set_style_text_color(replay_lbl, theme()->text_primary, 0);
	lv_label_set_text(replay_lbl, ru() ? "Ещё раз" : "Replay");
	lv_obj_center(replay_lbl);
	lv_obj_set_hidden(replay_btn, true);

	load_spinner = spinner_create(page, &icon_loader_big);
	lv_obj_set_hidden(load_spinner, true);

	status_label = lv_label_create(page);
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_18, 0);
	lv_obj_set_style_text_color(status_label, theme()->text_secondary, 0);
	lv_label_set_text(status_label, "");
	lv_obj_set_hidden(status_label, true);

	for (int i = 0; i < CHOICES; i++) {
		choice_btn[i] = make_choice(page, i);
		lv_obj_set_hidden(choice_btn[i], true);
	}

	feedback_label = lv_label_create(page);
	lv_label_set_long_mode(feedback_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(feedback_label, lv_pct(100));
	lv_obj_set_style_text_font(feedback_label, &font_ui_20, 0);
	lv_obj_set_style_text_color(feedback_label, theme()->text_primary, 0);
	lv_obj_set_style_text_align(feedback_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(feedback_label, "");
	lv_obj_set_hidden(feedback_label, true);

	// Full-screen track preview: sits on the quiz screen above the page, tap or
	// timer advances to the next round.
	compute_reveal_cover();
	reveal_box = lv_obj_create(quiz_screen);
	lv_obj_remove_style_all(reveal_box);
	lv_obj_set_size(reveal_box, lv_pct(100), lv_pct(100));
	lv_obj_align(reveal_box, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_set_style_bg_color(reveal_box, theme()->screen_bg, 0);
	lv_obj_set_style_bg_opa(reveal_box, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(reveal_box, pad, 0);
	lv_obj_set_style_pad_ver(reveal_box, SC(24), 0);
	lv_obj_set_flex_flow(reveal_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(reveal_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(reveal_box, SC(16), 0);
	lv_obj_set_scrollable(reveal_box, false);
	lv_obj_set_clickable(reveal_box, true);
	lv_obj_add_event_cb(reveal_box, reveal_tap_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(reveal_box, true);

	reveal_result = lv_label_create(reveal_box);
	lv_obj_set_width(reveal_result, lv_pct(100));
	lv_obj_set_style_text_align(reveal_result, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(reveal_result, &font_ui_22, 0);
	lv_obj_set_style_text_color(reveal_result, theme()->accent, 0);
	lv_label_set_text(reveal_result, "");

	reveal_cover_img = lv_image_create(reveal_box);
	lv_obj_set_size(reveal_cover_img, reveal_cover_px, reveal_cover_px);
	lv_obj_set_hidden(reveal_cover_img, true);

	reveal_title = lv_label_create(reveal_box);
	lv_label_set_long_mode(reveal_title, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(reveal_title, lv_pct(100));
	lv_obj_set_style_text_align(reveal_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(reveal_title, &font_ui_26, 0);
	lv_obj_set_style_text_color(reveal_title, theme()->text_primary, 0);

	reveal_meta = lv_label_create(reveal_box);
	lv_label_set_long_mode(reveal_meta, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(reveal_meta, lv_pct(100));
	lv_obj_set_style_text_align(reveal_meta, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(reveal_meta, &font_ui_20, 0);
	lv_obj_set_style_text_color(reveal_meta, theme()->text_secondary, 0);

	// Timed-run results: status → hero score → two stats → CTA.
	results_box = lv_obj_create(quiz_screen);
	lv_obj_remove_style_all(results_box);
	lv_obj_set_size(results_box, lv_pct(100), lv_pct(100));
	lv_obj_align(results_box, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_set_style_bg_color(results_box, theme()->screen_bg, 0);
	lv_obj_set_style_bg_opa(results_box, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(results_box, pad, 0);
	lv_obj_set_style_pad_ver(results_box, SC(24), 0);
	lv_obj_set_flex_flow(results_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(results_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(results_box, false);
	lv_obj_set_clickable(results_box, true);
	lv_obj_set_hidden(results_box, true);

	results_card = lv_obj_create(results_box);
	lv_obj_remove_style_all(results_card);
	lv_obj_set_width(results_card, lv_pct(100));
	lv_obj_set_height(results_card, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(results_card, theme()->surface, 0);
	lv_obj_set_style_bg_opa(results_card, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(results_card, SC(20), 0);
	lv_obj_set_style_pad_all(results_card, SC(28), 0);
	lv_obj_set_style_pad_row(results_card, SC(8), 0);
	lv_obj_set_flex_flow(results_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(results_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(results_card, false);

	results_title = lv_label_create(results_card);
	lv_obj_set_width(results_title, lv_pct(100));
	lv_obj_set_style_text_align(results_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(results_title, &font_ui_18, 0);
	lv_obj_set_style_text_color(results_title, theme()->text_secondary, 0);
	lv_label_set_text(results_title, "");

	results_score = lv_label_create(results_card);
	lv_obj_set_width(results_score, lv_pct(100));
	lv_obj_set_style_text_align(results_score, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(results_score, &font_ui_26, 0);
	lv_obj_set_style_text_color(results_score, theme()->accent, 0);
	lv_obj_set_style_pad_top(results_score, SC(12), 0);
	lv_label_set_text(results_score, "");

	results_score_cap = lv_label_create(results_card);
	lv_obj_set_width(results_score_cap, lv_pct(100));
	lv_obj_set_style_text_align(results_score_cap, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(results_score_cap, &font_ui_18, 0);
	lv_obj_set_style_text_color(results_score_cap, theme()->text_secondary, 0);
	lv_label_set_text(results_score_cap, "");

	results_stats_row = lv_obj_create(results_card);
	lv_obj_remove_style_all(results_stats_row);
	lv_obj_set_width(results_stats_row, lv_pct(100));
	lv_obj_set_height(results_stats_row, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(results_stats_row, SC(20), 0);
	lv_obj_set_style_pad_bottom(results_stats_row, SC(8), 0);
	lv_obj_set_style_pad_column(results_stats_row, SC(12), 0);
	lv_obj_set_flex_flow(results_stats_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(results_stats_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(results_stats_row, false);

	results_stat_correct = lv_obj_create(results_stats_row);
	lv_obj_remove_style_all(results_stat_correct);
	lv_obj_set_flex_grow(results_stat_correct, 1);
	lv_obj_set_height(results_stat_correct, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(results_stat_correct, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(results_stat_correct, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(results_stat_correct, SC(4), 0);

	results_stat_correct_val = lv_label_create(results_stat_correct);
	lv_obj_set_style_text_font(results_stat_correct_val, &font_ui_26, 0);
	lv_obj_set_style_text_color(results_stat_correct_val, theme()->text_primary, 0);
	lv_label_set_text(results_stat_correct_val, "");

	results_stat_correct_lbl = lv_label_create(results_stat_correct);
	lv_obj_set_style_text_font(results_stat_correct_lbl, &font_ui_18, 0);
	lv_obj_set_style_text_color(results_stat_correct_lbl, theme()->text_secondary, 0);
	lv_label_set_text(results_stat_correct_lbl, "");

	results_stat_lives = lv_obj_create(results_stats_row);
	lv_obj_remove_style_all(results_stat_lives);
	lv_obj_set_flex_grow(results_stat_lives, 1);
	lv_obj_set_height(results_stat_lives, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(results_stat_lives, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(results_stat_lives, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(results_stat_lives, SC(4), 0);

	results_stat_lives_val = lv_label_create(results_stat_lives);
	lv_obj_set_style_text_font(results_stat_lives_val, &font_ui_26, 0);
	lv_obj_set_style_text_color(results_stat_lives_val, theme()->text_primary, 0);
	lv_label_set_text(results_stat_lives_val, "");

	results_stat_lives_lbl = lv_label_create(results_stat_lives);
	lv_obj_set_style_text_font(results_stat_lives_lbl, &font_ui_18, 0);
	lv_obj_set_style_text_color(results_stat_lives_lbl, theme()->text_secondary, 0);
	lv_label_set_text(results_stat_lives_lbl, "");

	results_btn = lv_btn_create(results_card);
	lv_obj_set_width(results_btn, lv_pct(100));
	lv_obj_set_height(results_btn, CHOICE_H);
	lv_obj_set_style_radius(results_btn, SC(14), 0);
	lv_obj_set_style_shadow_width(results_btn, 0, 0);
	lv_obj_set_style_border_width(results_btn, 0, 0);
	lv_obj_set_style_bg_color(results_btn, theme()->accent, 0);
	lv_obj_set_style_bg_opa(results_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_margin_top(results_btn, SC(20), 0);
	lv_obj_add_event_cb(results_btn, results_restart_cb, LV_EVENT_CLICKED, NULL);

	results_btn_lbl = lv_label_create(results_btn);
	lv_obj_set_style_text_font(results_btn_lbl, &font_ui_22, 0);
	lv_obj_set_style_text_color(results_btn_lbl, lv_color_white(), 0);
	lv_label_set_text(results_btn_lbl, ru() ? "Ещё раз" : "Play again");
	lv_obj_center(results_btn_lbl);

	snippet_timer = lv_timer_create(snippet_done_cb, SNIPPET_MS, NULL);
	lv_timer_pause(snippet_timer);
	power_pause_in_standby(snippet_timer);

	seek_timer = lv_timer_create(seek_settle_cb, SEEK_SETTLE_MS, NULL);
	lv_timer_pause(seek_timer);
	power_pause_in_standby(seek_timer);

	answer_timer = lv_timer_create(answer_tick_cb, ANSWER_TICK_MS, NULL);
	lv_timer_pause(answer_timer);
	power_pause_in_standby(answer_timer);

	reveal_timer = lv_timer_create(reveal_done_cb, 3000, NULL);
	lv_timer_pause(reveal_timer);
	power_pause_in_standby(reveal_timer);

	feedback_timer = lv_timer_create(feedback_tick_cb, FEEDBACK_HALF_MS, NULL);
	lv_timer_pause(feedback_timer);
	power_pause_in_standby(feedback_timer);

	poll_timer = lv_timer_create(poll_cb, 200, NULL);
	lv_timer_pause(poll_timer);
	power_pause_in_standby(poll_timer);

	lv_obj_add_event_cb(quiz_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(quiz_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(quiz_screen);

	build_settings();
	build_records();
	show_idle();

	if (!theme_hook) {
		theme_register_refresh(theme_cb);
		theme_hook = true;
	}
	return quiz_screen;
}

static bool quiz_addon_stop(void) {
	job_gen++;
	stop_snippet();
	stop_answer_timer();
	hide_reveal();
	if (snippet_timer) {
		power_forget_timer(snippet_timer);
		lv_timer_delete(snippet_timer);
		snippet_timer = NULL;
	}
	if (seek_timer) {
		power_forget_timer(seek_timer);
		lv_timer_delete(seek_timer);
		seek_timer = NULL;
	}
	if (answer_timer) {
		power_forget_timer(answer_timer);
		lv_timer_delete(answer_timer);
		answer_timer = NULL;
	}
	if (reveal_timer) {
		power_forget_timer(reveal_timer);
		lv_timer_delete(reveal_timer);
		reveal_timer = NULL;
	}
	if (feedback_timer) {
		power_forget_timer(feedback_timer);
		lv_timer_delete(feedback_timer);
		feedback_timer = NULL;
	}
	feedback_toggles_left = 0;
	feedback_holding = false;
	feedback_ok_idx = -1;
	if (poll_timer) {
		power_forget_timer(poll_timer);
		lv_timer_delete(poll_timer);
		poll_timer = NULL;
	}
	if (theme_hook) {
		theme_unregister_refresh(theme_cb);
		theme_hook = false;
	}
	if (quiz_screen) {
		switcher_forget_screen(quiz_screen);
	}
	if (settings_screen) {
		switcher_forget_screen(settings_screen);
	}
	if (records_screen) {
		switcher_forget_screen(records_screen);
	}

	quiz_screen = NULL;
	settings_screen = NULL;
	records_screen = NULL;
	records_surv_val = NULL;
	records_end_val = NULL;
	records_timed_val = NULL;
	prompt_label = NULL;
	score_label = NULL;
	for (int i = 0; i < LIVES_MAX; i++) {
		if (lives_img[i]) {
			lv_image_set_src(lives_img[i], NULL);
		}
		lives_img[i] = NULL;
	}
	lives_row = NULL;
	if (heart_full_px) {
		stbi_image_free(heart_full_px);
		heart_full_px = NULL;
	}
	if (heart_empty_px) {
		stbi_image_free(heart_empty_px);
		heart_empty_px = NULL;
	}
	memset(&heart_full_dsc, 0, sizeof(heart_full_dsc));
	memset(&heart_empty_dsc, 0, sizeof(heart_empty_dsc));
	hearts_ready = false;
	if (trophy_px) {
		stbi_image_free(trophy_px);
		trophy_px = NULL;
	}
	memset(&trophy_dsc, 0, sizeof(trophy_dsc));
	trophy_ready = false;
	trophy_icon = NULL;
	timer_label = NULL;
	feedback_label = NULL;
	replay_btn = NULL;
	replay_lbl = NULL;
	meta_row = NULL;
	idle_box = NULL;
	idle_title = NULL;
	idle_ask_cap = NULL;
	idle_ask_row = NULL;
	idle_play_cap = NULL;
	idle_play_row = NULL;
	idle_mode_blurb = NULL;
	for (int i = 0; i < MODE_COUNT; i++) {
		idle_ask_pills[i] = NULL;
	}
	for (int i = 0; i < PLAY_COUNT; i++) {
		idle_play_pills[i] = NULL;
	}
	start_btn = NULL;
	start_lbl = NULL;
	load_spinner = NULL;
	status_label = NULL;
	reveal_box = NULL;
	reveal_cover_img = NULL;
	reveal_result = NULL;
	reveal_title = NULL;
	reveal_meta = NULL;
	results_box = NULL;
	results_card = NULL;
	results_title = NULL;
	results_score = NULL;
	results_score_cap = NULL;
	results_stats_row = NULL;
	results_stat_correct = NULL;
	results_stat_correct_val = NULL;
	results_stat_correct_lbl = NULL;
	results_stat_lives = NULL;
	results_stat_lives_val = NULL;
	results_stat_lives_lbl = NULL;
	results_btn = NULL;
	results_btn_lbl = NULL;
	for (int i = 0; i < CHOICES; i++) {
		choice_btn[i] = NULL;
		choice_lbl[i] = NULL;
	}
	for (int i = 0; i < MODE_COUNT; i++) {
		set_ask_pills[i] = NULL;
	}
	for (int i = 0; i < PLAY_COUNT; i++) {
		set_play_pills[i] = NULL;
	}
	set_survival_box = NULL;
	set_survival_lives_card = set_survival_lives_val = set_survival_lives_slider = NULL;
	set_timed_box = NULL;
	set_timed_run_card = set_timed_run_val = set_timed_run_slider = NULL;
	set_timed_limit_card = set_timed_limit_val = set_timed_limit_slider = NULL;
	set_timed_lives_card = set_timed_lives_val = set_timed_lives_slider = NULL;
	reveal_card = reveal_sw = reveal_val = reveal_slider = NULL;
	gui_cfg = NULL;
	addon_id = NULL;
	addon_dir = NULL;
	busy = false;
	answered = false;
	revealing = false;
	game_started = false;
	round_correct = -1;
	score = 0;
	streak = 0;
	best_streak = 0;
	run_correct = 0;
	run_end_failed = false;
	lives_left = 0;
	memset(choice_missed, 0, sizeof(choice_missed));
	run_active = false;
	run_needs_again = false;
	last_result_line[0] = '\0';
	we_started_playback = false;
	invalidate_pool();
	store_ready = false;
	best_survival_score = 0;
	best_endless_streak = 0;
	best_timed_score = 0;
	snippet_path[0] = '\0';
	memset(round_tracks, 0, sizeof(round_tracks));
	memset(pool, 0, sizeof(pool));
	return true;
}

static const sonix_addon_t quiz = {
	.api = SONIX_ADDON_API,
	.title = "Quiz",
	.init = quiz_addon_init,
};

static const sonix_addon_stop_t quiz_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = quiz_addon_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &quiz : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &quiz_lifecycle : NULL;
}
