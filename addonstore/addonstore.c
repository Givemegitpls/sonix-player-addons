#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "asdata.h"
#include "asinst.h"
#include "asnet.h"

#include "common/addonfs.h"

#include "src/addon/addon.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/scale.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

#include "lvgl/lvgl.h"

// The addon store: what this card has, what a GitHub repository publishes, and
// the one tap between them.
//
// Three pages:
//
//   list    one row per addon -- installed, published, or both -- with the
//           status in the value column: the version on the card, "update", or
//           "install". A tap opens the detail page;
//   detail  the versions, the release file, and the actions: install or
//           update, delete (behind the shell's confirmation), README;
//   readme  the addon's own README.txt, wrapped and scrolling.
//
// The card is the truth about what is installed: installed addons are read from
// Addons/*/addon.json every time a page is built, so a folder copied over USB is
// there and one deleted by hand is gone. installed.tsv only remembers which
// release a folder came from, which is what makes "update" mean "the published
// asset is not the one that was installed".
//
// One job at a time, on a worker; the page polls it with an LVGL timer, so
// nothing here calls into LVGL from another thread. stop() sets the cancel flag,
// waits a bounded time for the worker, and answers whether the library may be
// unmapped -- false while a request is still on the wire, which is the honest
// answer addon.h asks for.
//
// What it cannot do: make a newly installed tile appear. The player rescans
// Addons/ when the card is attached, and addon_scan() is not part of the ABI, so
// the page says to reinsert the card (or restart) instead of pretending.

#define POLL_MS 400
#define STOP_WAIT_MS 3000
#define README_TEXT_MAX (64 * 1024)

typedef enum {
	JOB_NONE = 0,
	JOB_REFRESH,
	JOB_INSTALL,
	JOB_README,
	JOB_UPDATE_ALL,
} job_kind_t;

typedef struct {
	char id[AS_ID_MAX];
} row_ref_t;

typedef struct {
	char repo[AS_REPO_MAX];
} repo_ref_t;

typedef struct {
	int count;
	as_release_t rel[AS_MAX];
} refresh_result_t;

// What "Update all" did, per addon. The worker only fills this; the records
// are written on the UI thread, because installed.tsv is the page's own file.
typedef struct {
	char id[AS_ID_MAX];
	bool ok;
	char version[AS_VERSION_MAX];
	char error[AS_ERROR_MAX];
} update_result_t;

typedef struct {
	int count;
	update_result_t item[AS_MAX];
} update_result_set_t;

typedef enum {
	ENTRY_SEARCH = 0,
	ENTRY_REPO,
} entry_mode_t;

static const sonix_addon_host_t *host;
static gui_config_t *cfg;
static const char *addon_id;
static as_model_t model;

static lv_obj_t *list_screen;
static lv_obj_t *list_container;
static lv_obj_t *list_status; // the sentence under the rows: an error, or "checking"
static lv_obj_t *detail_screen;
static lv_obj_t *detail_title;
static lv_obj_t *detail_container;
static lv_obj_t *detail_status;
static lv_obj_t *readme_screen;
static lv_obj_t *readme_container;
static lv_obj_t *readme_title;
static lv_obj_t *repos_screen;
static lv_obj_t *repos_container;
static lv_obj_t *repos_status;
static lv_obj_t *entry_screen;
static lv_obj_t *entry_field;
static lv_obj_t *entry_title;
static keyboard_t *entry_kb;
static entry_mode_t entry_mode;

static row_ref_t rows[AS_MAX];
static int row_count;
static repo_ref_t repo_rows[AS_REPOS_MAX];
static int repo_row_count;
static char detail_id[AS_ID_MAX];
static char query[AS_QUERY_MAX];

// The job, and the one lock between the worker and the UI thread. The worker
// fills `job_*` and sets job_done last; the poll timer reads them after that.
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static int workers_live;
static bool stopping;
static as_progress_t progress;
static job_kind_t job_kind;
static bool job_done;
static bool job_ok;
static char job_error[AS_ERROR_MAX];
static char job_version[AS_VERSION_MAX];
static as_release_t job_release;
static refresh_result_t *job_refresh;
static update_result_set_t *job_updates;
static char job_step_id[AS_ID_MAX]; // the addon the update-all worker is on
static int job_step;
static int job_steps;

// The "Update all" plan, made on the UI thread and walked by the worker: ids
// only, resolved against the (unchanged while busy) cache, so this costs a few
// kilobytes rather than a copy of every release.
static char update_plan[AS_MAX][AS_ID_MAX];
static int update_plan_count;

static lv_timer_t *poll_timer;
static bool busy; // UI thread only: a job is running and the timer is awake
static bool refreshed_this_session;
static volatile bool job_reading; // a refresh is reading manifests, not the API

// ---------------------------------------------------------------------------
// Words
// ---------------------------------------------------------------------------

static bool ru(void) {
	const char *name = lang_current();
	return name && strcmp(name, "Russian") == 0;
}

static const char *t(const char *en, const char *rus) {
	return ru() ? rus : en;
}

static void human_size(char *out, size_t size, long bytes) {
	if (bytes >= 1024L * 1024L) {
		lv_snprintf(out, size, "%ld.%ld MB", bytes / (1024L * 1024L), (bytes % (1024L * 1024L)) / (1024L * 102));
	} else if (bytes >= 1024L) {
		lv_snprintf(out, size, "%ld KB", (bytes + 1023) / 1024);
	} else {
		lv_snprintf(out, size, "%ld B", bytes);
	}
}

// "dino" -> "Dino", for a release the card does not have yet.
static void pretty_name(const char *id, char *out, size_t size) {
	lv_snprintf(out, size, "%s", id);
	if (out[0] >= 'a' && out[0] <= 'z') {
		out[0] = (char)(out[0] - 'a' + 'A');
	}
	for (char *p = out + 1; *p; p++) {
		if (*p == '_' || *p == '-') {
			*p = ' ';
		}
	}
}

static const char *title_of(const char *id, char *scratch, size_t size) {
	const as_installed_t *inst = as_installed_find(&model, id);
	if (inst && inst->title[0]) {
		return inst->title;
	}
	const as_release_t *rel = as_release_find(&model, id);
	if (rel && rel->title[0]) {
		return rel->title;
	}
	pretty_name(id, scratch, size);
	return scratch;
}

// The player's four pages, in the order the shell shows them, and the bucket for
// a release whose manifest has not been read (or names a section this player
// does not have). Empty buckets are not drawn at all.
static const struct {
	const char *key;
	const char *en;
	const char *ru;
} sections[] = {
	{"music", "Music", "Музыка"},
	{"streaming", "Streaming", "Стриминг"},
	{"wireless", "Wireless", "Беспроводные"},
	{"more", "More", "Ещё"},
	{"", "Other", "Прочее"},
};

#define SECTION_COUNT ((int)(sizeof(sections) / sizeof(sections[0])))

static const char *section_of(const char *id) {
	const as_installed_t *inst = as_installed_find(&model, id);
	if (inst && inst->section[0]) {
		return inst->section;
	}
	const as_release_t *rel = as_release_find(&model, id);
	if (rel && rel->section[0]) {
		return rel->section;
	}
	return "";
}

static lv_obj_t *section_header(lv_obj_t *parent, const char *text) {
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_font(label, &font_ui_18, 0);
	lv_obj_set_style_text_color(label, theme()->text_secondary, 0);
	lv_obj_set_style_pad_top(label, SC(10), 0);
	lv_obj_set_style_pad_left(label, SC(4), 0);
	lv_label_set_text(label, text);
	return label;
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

static void job_finish(bool ok, const char *error) {
	pthread_mutex_lock(&job_lock);
	job_ok = ok;
	lv_snprintf(job_error, sizeof(job_error), "%s", error ? error : "");
	job_done = true;
	pthread_mutex_unlock(&job_lock);
}

static void worker_started(void) {
	pthread_mutex_lock(&job_lock);
	workers_live++;
	pthread_mutex_unlock(&job_lock);
}

static void worker_ended(void) {
	pthread_mutex_lock(&job_lock);
	workers_live--;
	pthread_cond_broadcast(&job_cond);
	pthread_mutex_unlock(&job_lock);
}

static void *refresh_worker(void *unused) {
	(void)unused;
	refresh_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		job_finish(false, "out of memory");
		worker_ended();
		return NULL;
	}
	char first_error[AS_ERROR_MAX] = "";
	bool any = false;
	for (int i = 0; i < model.repo_count; i++) {
		if (progress.stop) {
			break;
		}
		as_release_t found[AS_MAX];
		int count = 0;
		char error[AS_ERROR_MAX] = "";
		if (as_fetch_releases(model.repo[i], found, AS_MAX, &count, error, sizeof(error), &progress)) {
			any = true;
			for (int j = 0; j < count && result->count < AS_MAX; j++) {
				bool duplicate = false;
				for (int k = 0; k < result->count; k++) {
					if (strcmp(result->rel[k].id, found[j].id) == 0) {
						duplicate = true;
					}
				}
				if (!duplicate) {
					result->rel[result->count++] = found[j];
				}
			}
		} else if (!first_error[0]) {
			lv_snprintf(first_error, sizeof(first_error), "%s: %s", model.repo[i], error);
		}
	}
	// A published file that has not changed keeps what its manifest said last
	// time: the cache is the whole point of not downloading it again. An entry
	// that came from the repository's index already has all of it.
	for (int j = 0; j < result->count; j++) {
		if (result->rel[j].listed) {
			continue;
		}
		const as_release_t *old = as_release_find(&model, result->rel[j].id);
		if (old && old->size == result->rel[j].size && strcmp(old->digest, result->rel[j].digest) == 0) {
			memcpy(result->rel[j].title, old->title, sizeof(result->rel[j].title));
			memcpy(result->rel[j].section, old->section, sizeof(result->rel[j].section));
			memcpy(result->rel[j].so_sha256, old->so_sha256, sizeof(result->rel[j].so_sha256));
			result->rel[j].so_size = old->so_size;
			memcpy(result->rel[j].version, old->version, sizeof(result->rel[j].version));
		}
	}

	// The rest are read out of their own zip, once per published file. The
	// download does not count against GitHub's API limit, and this is how a
	// release the card does not have gets into the right section.
	int missing = 0;
	for (int j = 0; j < result->count; j++) {
		if (!result->rel[j].section[0]) {
			missing++;
		}
	}
	if (missing > 0 && !progress.stop) {
		job_reading = true;
		progress.total = 0;
		for (int j = 0; j < result->count && !progress.stop; j++) {
			if (result->rel[j].section[0]) {
				continue;
			}
			char why[AS_ERROR_MAX] = "";
			if (!as_read_manifest(&model, &result->rel[j], &progress, why, sizeof(why))) {
				printf("addonstore: %s manifest not read: %s\n", result->rel[j].id, why);
			}
		}
		job_reading = false;
	}

	printf("addonstore: refresh done, %d entries%s%s\n", result->count, first_error[0] ? ", first error: " : "",
		   first_error);
	pthread_mutex_lock(&job_lock);
	job_refresh = result;
	if (progress.stop) {
		job_ok = false;
		lv_snprintf(job_error, sizeof(job_error), "%s", "cancelled");
	} else {
		job_ok = any;
		lv_snprintf(job_error, sizeof(job_error), "%s", any ? "" : first_error);
	}
	job_done = true;
	pthread_mutex_unlock(&job_lock);
	worker_ended();
	return NULL;
}

static void *install_worker(void *unused) {
	(void)unused;
	as_release_t release;
	pthread_mutex_lock(&job_lock);
	release = job_release;
	pthread_mutex_unlock(&job_lock);

	char error[AS_ERROR_MAX] = "";
	char version[AS_VERSION_MAX] = "";
	bool ok = as_install(&model, &release, &progress, version, sizeof(version), error, sizeof(error));
	printf("addonstore: install %s (%ld bytes) %s%s%s\n", release.id, release.size, ok ? "ok" : "failed",
		   error[0] ? ": " : "", error);
	pthread_mutex_lock(&job_lock);
	lv_snprintf(job_version, sizeof(job_version), "%s", version);
	pthread_mutex_unlock(&job_lock);
	job_finish(ok, error);
	worker_ended();
	return NULL;
}

static void *readme_worker(void *unused) {
	(void)unused;
	as_release_t release;
	pthread_mutex_lock(&job_lock);
	release = job_release;
	pthread_mutex_unlock(&job_lock);

	char error[AS_ERROR_MAX] = "";
	bool ok = as_readme_fetch(&model, &release, release.id, &progress, error, sizeof(error));
	printf("addonstore: readme %s %s%s%s\n", release.id, ok ? "ok" : "failed", error[0] ? ": " : "", error);
	job_finish(ok, error);
	worker_ended();
	return NULL;
}

// Update all: the plan was made on the UI thread (update_plan) and this only
// walks it. The store's own release is last -- its library is the one this page
// is running from, and the others should land even if that one cannot.
static void *update_all_worker(void *unused) {
	(void)unused;
	update_result_set_t *result = calloc(1, sizeof(*result));
	if (!result) {
		job_finish(false, "out of memory");
		worker_ended();
		return NULL;
	}
	char first_error[AS_ERROR_MAX] = "";
	pthread_mutex_lock(&job_lock);
	job_steps = update_plan_count;
	job_step = 0;
	pthread_mutex_unlock(&job_lock);

	for (int i = 0; i < update_plan_count && !progress.stop; i++) {
		const as_release_t *release = as_release_find(&model, update_plan[i]);
		if (!release) {
			continue;
		}
		pthread_mutex_lock(&job_lock);
		job_step = i;
		lv_snprintf(job_step_id, sizeof(job_step_id), "%s", release->id);
		pthread_mutex_unlock(&job_lock);

		update_result_t *r = &result->item[result->count];
		memset(r, 0, sizeof(*r));
		lv_snprintf(r->id, sizeof(r->id), "%s", release->id);
		char error[AS_ERROR_MAX] = "";
		r->ok = as_install(&model, release, &progress, r->version, sizeof(r->version), error, sizeof(error));
		if (!r->ok) {
			lv_snprintf(r->error, sizeof(r->error), "%s", error);
			if (!first_error[0]) {
				lv_snprintf(first_error, sizeof(first_error), "%s: %s", r->id, error);
			}
		}
		result->count++;
		printf("addonstore: update-all %s %s%s%s\n", r->id, r->ok ? "ok" : "failed", error[0] ? ": " : "", error);
	}

	pthread_mutex_lock(&job_lock);
	job_updates = result;
	job_step = update_plan_count;
	job_ok = !progress.stop && result->count > 0;
	lv_snprintf(job_error, sizeof(job_error), "%s", progress.stop ? "cancelled" : first_error);
	job_done = true;
	pthread_mutex_unlock(&job_lock);
	worker_ended();
	return NULL;
}

static bool job_start(job_kind_t kind, const as_release_t *release) {
	if (busy || stopping) {
		return false;
	}
	pthread_mutex_lock(&job_lock);
	job_kind = kind;
	job_done = false;
	job_ok = false;
	job_error[0] = '\0';
	job_version[0] = '\0';
	job_refresh = NULL;
	job_updates = NULL;
	job_step = 0;
	job_steps = 0;
	job_step_id[0] = '\0';
	if (release) {
		job_release = *release;
	} else {
		memset(&job_release, 0, sizeof(job_release));
	}
	progress.done = 0;
	progress.total = 0;
	progress.stop = false;
	job_reading = false;
	pthread_mutex_unlock(&job_lock);

	void *(*fn)(void *) = refresh_worker;
	if (kind == JOB_INSTALL) {
		fn = install_worker;
	} else if (kind == JOB_README) {
		fn = readme_worker;
	} else if (kind == JOB_UPDATE_ALL) {
		fn = update_all_worker;
	}
	worker_started();
	pthread_t thread;
	if (pthread_create(&thread, NULL, fn, NULL) != 0) {
		worker_ended();
		pthread_mutex_lock(&job_lock);
		job_kind = JOB_NONE;
		pthread_mutex_unlock(&job_lock);
		return false;
	}
	pthread_detach(thread);
	busy = true;
	if (poll_timer) {
		lv_timer_resume(poll_timer);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

static lv_obj_t *scrolling_page(lv_obj_t *screen, const char *title, lv_obj_t **title_out) {
	lv_obj_add_style(screen, &theme_style_screen, 0);
	lv_obj_t *heading = settingsrow_title(screen, cfg, title);
	if (title_out) {
		*title_out = heading;
	}

	lv_obj_t *container = lv_obj_create(screen);
	lv_obj_set_size(container, lv_pct(100), cfg->screen_height - settingsrow_content_top(cfg));
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, settingsrow_content_top(cfg));
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
	player_sheet_attach_drag(screen, true);
	player_sheet_attach_drag(container, true);
	return container;
}

static void set_status(lv_obj_t *label, const char *text) {
	if (!label) {
		return;
	}
	if (text && text[0]) {
		lv_label_set_text(label, text);
		lv_obj_set_hidden(label, false);
	} else {
		lv_label_set_text(label, "");
		lv_obj_set_hidden(label, true);
	}
}

static lv_obj_t *status_label(lv_obj_t *parent) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_font(label, &font_ui_18, 0);
	lv_obj_set_style_text_color(label, theme()->text_secondary, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
	lv_label_set_text(label, "");
	lv_obj_set_hidden(label, true);
	return label;
}

// What the row's value says, and whether it is worth the accent colour.
static const char *row_value(const char *id, bool *highlight) {
	static char version[AS_VERSION_MAX];
	const as_installed_t *inst = as_installed_find(&model, id);
	const as_release_t *rel = as_release_find(&model, id);
	*highlight = false;
	if (inst && as_update_available(&model, id)) {
		*highlight = true;
		return t("update", "обновить");
	}
	if (inst) {
		// The record's version, not the manifest's: an update keeps addon.json
		// as it is, so the file on the card would still say what it said when
		// the folder was first copied there.
		as_installed_version(&model, id, version, sizeof(version));
		return version[0] ? version : t("installed", "установлен");
	}
	if (rel) {
		*highlight = true;
		return t("install", "установить");
	}
	return "";
}

static void open_detail(const char *id);
static void open_readme_page(const char *id);
static void build_list(void);
static void build_repos(void);
static void entry_open(entry_mode_t mode);
static void set_refresh_status(const char *text);

static void row_cb(lv_event_t *e) {
	row_ref_t *ref = lv_event_get_user_data(e);
	if (ref && ref->id[0]) {
		open_detail(ref->id);
	}
}

static void refresh_cb(lv_event_t *e) {
	(void)e;
	if (busy) {
		return;
	}
	if (job_start(JOB_REFRESH, NULL)) {
		set_refresh_status(t("asking GitHub…", "запрашиваю GitHub…"));
	}
}

static void clear_query_cb(lv_event_t *e) {
	(void)e;
	query[0] = '\0';
	build_list();
}

static void search_cb(lv_event_t *e) {
	(void)e;
	entry_open(ENTRY_SEARCH);
}

static void repos_cb(lv_event_t *e) {
	(void)e;
	build_repos();
	if (repos_screen) {
		switch_screen(repos_screen);
	}
}

static void add_repo_cb(lv_event_t *e) {
	(void)e;
	entry_open(ENTRY_REPO);
}

static void repos_refresh_cb(lv_event_t *e) {
	(void)e;
	if (busy) {
		return;
	}
	if (job_start(JOB_REFRESH, NULL)) {
		set_refresh_status(t("asking GitHub…", "запрашиваю GitHub…"));
	}
}

// How many released addons are newer than the card. Cheap now that the card's
// libraries are fingerprinted once (as_local_sha), and it is what the "Update
// all" row counts.
static int updates_available(void) {
	int count = 0;
	for (int i = 0; i < model.release_count; i++) {
		if (as_update_available(&model, model.release[i].id)) {
			count++;
		}
	}
	return count;
}

static void update_all_confirmed(void *user) {
	(void)user;
	if (busy || update_plan_count == 0) {
		return;
	}
	if (job_start(JOB_UPDATE_ALL, NULL)) {
		char line[64];
		lv_snprintf(line, sizeof(line), t("updating %d…", "обновляю %d…"), update_plan_count);
		set_status(list_status, line);
	}
}

static void update_all_cb(lv_event_t *e) {
	(void)e;
	if (busy) {
		toast_error(t("another job is running", "уже идёт другая задача"));
		return;
	}
	// The plan is made here, on the UI thread: two passes so the store's own
	// release is last, then one confirmation for the whole batch.
	update_plan_count = 0;
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < model.release_count && update_plan_count < AS_MAX; i++) {
			const char *id = model.release[i].id;
			bool own = addon_id && strcmp(id, addon_id) == 0;
			if (own != (pass == 1)) {
				continue;
			}
			if (!as_update_available(&model, id)) {
				continue;
			}
			lv_snprintf(update_plan[update_plan_count++], AS_ID_MAX, "%s", id);
		}
	}
	if (update_plan_count == 0) {
		toast_plain(t("everything is up to date", "всё актуально"));
		return;
	}
	char message[200];
	lv_snprintf(message, sizeof(message),
				t("%d addon(s) will be downloaded and replaced. Settings, tokens and saves stay.",
				  "%d аддон(ов) будут скачаны и заменены. Настройки, токены и сохранения останутся."),
				update_plan_count);
	confirm_show(t("Update everything?", "Обновить всё?"), message, t("update", "обновить"), update_all_confirmed,
				 NULL);
}

static void build_list(void) {
	if (!list_container) {
		return;
	}
	lv_obj_clean(list_container);
	row_count = 0;

	settingsrow_action(list_container, t("Search", "Поиск"), search_cb, NULL);
	int updates = updates_available();
	if (updates > 0) {
		char label[64];
		lv_snprintf(label, sizeof(label), t("Update all (%d)", "Обновить всё (%d)"), updates);
		settingsrow_action(list_container, label, update_all_cb, NULL);
	}
	settingsrow_action(list_container, t("Repositories", "Репозитории"), repos_cb, NULL);
	settingsrow_action(list_container, t("Check for updates", "Проверить обновления"), refresh_cb, NULL);
	if (query[0]) {
		char label[AS_QUERY_MAX + 40];
		lv_snprintf(label, sizeof(label), t("Clear search: %s", "Сбросить поиск: %s"), query);
		settingsrow_action(list_container, label, clear_query_cb, NULL);
	}

	// The union of what is installed and what is published, by id.
	char ids[AS_MAX][AS_ID_MAX];
	int count = 0;
	for (int i = 0; i < model.installed_count && count < AS_MAX; i++) {
		lv_snprintf(ids[count++], AS_ID_MAX, "%s", model.installed[i].id);
	}
	for (int i = 0; i < model.release_count && count < AS_MAX; i++) {
		bool seen = false;
		for (int j = 0; j < count; j++) {
			if (strcmp(ids[j], model.release[i].id) == 0) {
				seen = true;
			}
		}
		if (!seen) {
			lv_snprintf(ids[count++], AS_ID_MAX, "%s", model.release[i].id);
		}
	}
	// Case-insensitive by displayed title; a dozen rows, so a selection sort.
	char a[AS_TITLE_MAX], b[AS_TITLE_MAX];
	for (int i = 0; i < count; i++) {
		for (int j = i + 1; j < count; j++) {
			if (strcasecmp(title_of(ids[j], b, sizeof(b)), title_of(ids[i], a, sizeof(a))) < 0) {
				char swap[AS_ID_MAX];
				memcpy(swap, ids[i], sizeof(swap));
				memcpy(ids[i], ids[j], sizeof(swap));
				memcpy(ids[j], swap, sizeof(swap));
			}
		}
	}

	int shown = 0;
	for (int s = 0; s < SECTION_COUNT; s++) {
		bool header = false;
		for (int i = 0; i < count; i++) {
			if (strcmp(section_of(ids[i]), sections[s].key) != 0) {
				continue;
			}
			const char *title = title_of(ids[i], a, sizeof(a));
			if (!as_matches(ids[i], title, query)) {
				continue;
			}
			if (!header) {
				section_header(list_container, t(sections[s].en, sections[s].ru));
				header = true;
			}
			row_ref_t *ref = &rows[row_count++];
			lv_snprintf(ref->id, sizeof(ref->id), "%s", ids[i]);
			bool highlight = false;
			const char *status = row_value(ids[i], &highlight);
			lv_obj_t *value = NULL;
			settingsrow_add(list_container, title, &value, row_cb, ref);
			if (value) {
				lv_label_set_text(value, status);
				if (highlight) {
					lv_obj_set_style_text_color(value, theme()->accent, 0);
				}
			}
			shown++;
		}
	}

	list_status = status_label(list_container);
	if (query[0] && shown == 0) {
		set_status(list_status, t("nothing matches", "ничего не нашлось"));
	}
}

// ---------------------------------------------------------------------------
// Detail and README
// ---------------------------------------------------------------------------

static void install_cb(lv_event_t *e);

static void delete_confirmed(void *user) {
	(void)user;
	char error[AS_ERROR_MAX] = "";
	if (!as_remove(&model, detail_id, error, sizeof(error))) {
		printf("addonstore: remove %s failed: %s\n", detail_id, error);
		toast_error(error);
		return;
	}
	printf("addonstore: removed %s\n", detail_id);
	as_record_drop(&model, detail_id);
	as_scan_installed(&model);
	toast_success(t("removed", "удалено"));
	build_list();
	open_detail(detail_id);
}

static void delete_cb(lv_event_t *e) {
	(void)e;
	char message[200];
	lv_snprintf(message, sizeof(message), "%s\n%s",
				t("Its settings and caches in this folder go with it.",
				  "Его настройки и кэш в этой папке удалятся вместе с ним."),
				t("The tile goes away at the next card read.",
				  "Плитка исчезнет при следующем чтении карты."));
	confirm_show(t("Remove the addon?", "Удалить аддон?"), message, t("delete", "удалить"), delete_confirmed,
				 NULL);
}

static void readme_cb(lv_event_t *e) {
	(void)e;
	char path[1200];
	if (as_readme_local(&model, detail_id, path, sizeof(path)) ||
		as_readme_cached(&model, detail_id, path, sizeof(path))) {
		open_readme_page(detail_id);
		return;
	}
	const as_release_t *rel = as_release_find(&model, detail_id);
	if (!rel) {
		toast_error(t("no README to show", "README нет"));
		return;
	}
	if (job_start(JOB_README, rel)) {
		set_status(detail_status, t("fetching the README from the release…", "беру README из релиза…"));
	} else if (busy) {
		toast_error(t("another job is running", "уже идёт другая задача"));
	}
}

static void open_detail(const char *id) {
	lv_snprintf(detail_id, sizeof(detail_id), "%s", id);
	if (!detail_screen) {
		return;
	}
	as_scan_installed(&model);
	lv_obj_clean(detail_container);
	const as_installed_t *inst = as_installed_find(&model, id);
	const as_release_t *rel = as_release_find(&model, id);
	char title[AS_TITLE_MAX];
	lv_label_set_text(detail_title, title_of(id, title, sizeof(title)));

	lv_obj_t *value = NULL;
	settingsrow_add(detail_container, t("on the card", "на карте"), &value, NULL, NULL);
	if (value) {
		char shown[AS_VERSION_MAX];
		as_installed_version(&model, id, shown, sizeof(shown));
		lv_label_set_text(value, inst ? (shown[0] ? shown : t("yes", "есть")) : t("no", "нет"));
	}
	settingsrow_add(detail_container, t("in the release", "в релизе"), &value, NULL, NULL);
	if (value) {
		if (rel) {
			char size[32];
			char line[96];
			human_size(size, sizeof(size), rel->size);
			lv_snprintf(line, sizeof(line), "%s · %s", rel->tag[0] ? rel->tag : "?", size);
			lv_label_set_text(value, line);
		} else {
			lv_label_set_text(value, t("not published", "не найден"));
		}
	}

	if (rel) {
		if (as_installed_matches(&model, id)) {
			// Installed and identical to the release: nothing to do, and
			// saying so is more useful than offering a reinstall.
			lv_obj_t *match = NULL;
			settingsrow_add(detail_container, t("Update", "Обновление"), &match, NULL, NULL);
			if (match) {
				lv_label_set_text(match, t("up to date", "актуально"));
			}
		} else {
			settingsrow_action(detail_container, inst ? t("Update", "Обновить") : t("Install", "Установить"),
							   install_cb, NULL);
		}
	}
	if (inst) {
		settingsrow_action(detail_container, t("Delete", "Удалить"), delete_cb, NULL);
	}
	if (rel || inst) {
		settingsrow_action(detail_container, "README", readme_cb, NULL);
	}

	detail_status = status_label(detail_container);
	set_status(detail_status, "");
	switch_screen(detail_screen);
}

static void open_readme_page(const char *id) {
	if (!readme_screen) {
		return;
	}
	char path[1200];
	if (!as_readme_local(&model, id, path, sizeof(path)) && !as_readme_cached(&model, id, path, sizeof(path))) {
		toast_error(t("no README on the card yet", "README на карте ещё нет"));
		return;
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		toast_error(t("the README cannot be opened", "README не открывается"));
		return;
	}
	// Allocated to the file, not to the cap: this device has about a megabyte
	// of free RAM, and LVGL copies the text into the label as well. A README
	// is a few kilobytes; the cap only bounds a file somebody dropped in.
	long file_size = 0;
	if (fseek(f, 0, SEEK_END) == 0) {
		file_size = ftell(f);
		fseek(f, 0, SEEK_SET);
	}
	if (file_size <= 0) {
		file_size = README_TEXT_MAX;
	}
	if (file_size > README_TEXT_MAX) {
		file_size = README_TEXT_MAX;
	}
	char *text = malloc((size_t)file_size + 1);
	size_t len = text ? fread(text, 1, (size_t)file_size, f) : 0;
	fclose(f);
	if (!text) {
		toast_error(t("out of memory", "мало памяти"));
		return;
	}
	text[len] = '\0';

	lv_obj_clean(readme_container);
	lv_obj_t *label = lv_label_create(readme_container);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_font(label, &font_ui_18, 0);
	lv_obj_set_style_text_color(label, theme()->text_primary, 0);
	lv_label_set_text(label, text);
	free(text);

	char title[AS_TITLE_MAX];
	lv_label_set_text(readme_title, title_of(id, title, sizeof(title)));
	switch_screen(readme_screen);
}

// ---------------------------------------------------------------------------
// Repositories, and the one text field both "search" and "add a repository" use
// ---------------------------------------------------------------------------

static char pending_repo[AS_REPO_MAX];

// A refresh can be started from either page and finishes while either one is
// open, so its sentence goes to both.
static void set_refresh_status(const char *text) {
	set_status(list_status, text);
	set_status(repos_status, text);
}

static void repo_removed(void *user) {
	const char *name = user;
	for (int i = 0; i < model.repo_count; i++) {
		if (strcmp(model.repo[i], name) == 0) {
			as_repos_remove(&model, i);
			break;
		}
	}
	printf("addonstore: repo %s removed\n", name);
	build_repos();
	build_list();
	if (!busy && job_start(JOB_REFRESH, NULL)) {
		set_refresh_status(t("asking GitHub…", "запрашиваю GitHub…"));
	}
}

static void repo_row_cb(lv_event_t *e) {
	repo_ref_t *ref = lv_event_get_user_data(e);
	if (!ref || !ref->repo[0]) {
		return;
	}
	lv_snprintf(pending_repo, sizeof(pending_repo), "%s", ref->repo);
	char message[200];
	lv_snprintf(message, sizeof(message),
				t("Its addons stay on the card; only this repository stops being read.",
				  "Его аддоны останутся на карте, читаться перестанет только этот репозиторий."));
	confirm_show(t("Stop reading this repository?", "Больше не читать этот репозиторий?"), message,
				 t("remove", "удалить"), repo_removed, pending_repo);
}

static void build_repos(void) {
	if (!repos_container) {
		return;
	}
	lv_obj_clean(repos_container);
	repo_row_count = 0;
	for (int i = 0; i < model.repo_count && repo_row_count < AS_REPOS_MAX; i++) {
		repo_ref_t *ref = &repo_rows[repo_row_count++];
		lv_snprintf(ref->repo, sizeof(ref->repo), "%s", model.repo[i]);
		lv_obj_t *value = NULL;
		settingsrow_add(repos_container, model.repo[i], &value, repo_row_cb, ref);
		if (value) {
			lv_label_set_text(value, t("remove", "удалить"));
		}
	}
	settingsrow_action(repos_container, t("Add repository", "Добавить репозиторий"), add_repo_cb, NULL);
	settingsrow_action(repos_container, t("Check for updates", "Проверить обновления"), repos_refresh_cb, NULL);
	repos_status = status_label(repos_container);
}

// The field is shared: "search" and "add a repository" differ in the title, the
// placeholder and what accept does, not in the screen or the keyboard, and this
// device cannot afford a keyboard per page.
static void entry_open(entry_mode_t mode) {
	entry_mode = mode;
	if (!entry_screen) {
		return;
	}
	lv_label_set_text(entry_title,
					  mode == ENTRY_SEARCH ? t("Search", "Поиск") : t("Add repository", "Добавить репозиторий"));
	lv_textarea_set_placeholder_text(entry_field, mode == ENTRY_SEARCH ? t("name or id", "имя или id")
																	   : t("owner/name", "владелец/имя"));
	lv_textarea_set_text(entry_field, mode == ENTRY_SEARCH ? query : "");
	keyboard_reset(entry_kb);
	keyboard_set_visible(entry_kb, true);
	switch_screen(entry_screen);
}

static void trim_text(char *s) {
	char *p = s;
	while (*p == ' ') {
		p++;
	}
	if (p != s) {
		memmove(s, p, strlen(p) + 1);
	}
	size_t len = strlen(s);
	while (len > 0 && s[len - 1] == ' ') {
		s[--len] = '\0';
	}
}

static void entry_accept_cb(lv_event_t *e) {
	(void)e;
	if (!entry_field) {
		return;
	}
	char text[AS_URL_MAX];
	lv_snprintf(text, sizeof(text), "%s", lv_textarea_get_text(entry_field));
	trim_text(text);
	if (entry_mode == ENTRY_SEARCH) {
		lv_snprintf(query, sizeof(query), "%s", text);
		keyboard_set_visible(entry_kb, false);
		build_list();
		if (list_screen) {
			switch_screen(list_screen);
		}
		return;
	}
	if (busy) {
		toast_error(t("another job is running", "уже идёт другая задача"));
		return;
	}
	if (!as_repos_add(&model, text)) {
		toast_error(t("that is not owner/name", "это не owner/name"));
		return;
	}
	printf("addonstore: repo %s added\n", text);
	lv_textarea_set_text(entry_field, "");
	keyboard_set_visible(entry_kb, false);
	build_repos();
	if (repos_screen) {
		switch_screen(repos_screen);
	}
	if (!busy && job_start(JOB_REFRESH, NULL)) {
		set_refresh_status(t("asking GitHub…", "запрашиваю GitHub…"));
	}
}

// ---------------------------------------------------------------------------
// What the page does
// ---------------------------------------------------------------------------

static void install_cb(lv_event_t *e) {
	(void)e;
	// The store updates itself like any addon: the new addon.so is renamed
	// into place, which leaves the old inode mapped and running, and the
	// player opens the new one when the tile is tapped again. Deleting itself
	// stays refused -- there would be nothing left to open.
	const as_release_t *rel = as_release_find(&model, detail_id);
	if (!rel) {
		toast_error(t("nothing to install", "устанавливать нечего"));
		return;
	}
	if (job_start(JOB_INSTALL, rel)) {
		set_status(detail_status, t("downloading…", "скачиваю…"));
	} else if (busy) {
		toast_error(t("another job is running", "уже идёт другая задача"));
	}
}

static void list_loaded_cb(lv_event_t *e) {
	(void)e;
	as_scan_installed(&model);
	build_list();
	if (!busy && !refreshed_this_session && job_start(JOB_REFRESH, NULL)) {
		set_refresh_status(t("asking GitHub…", "запрашиваю GitHub…"));
	}
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (!busy) {
		return;
	}

	char line[128];
	if (job_kind == JOB_UPDATE_ALL) {
		int step = job_step + 1;
		if (step > job_steps) {
			step = job_steps;
		}
		if (progress.total > 0) {
			long percent = progress.done * 100 / progress.total;
			if (percent > 100) {
				percent = 100;
			}
			lv_snprintf(line, sizeof(line), t("%d/%d %s %ld%%", "%d/%d %s %ld%%"), step, job_steps, job_step_id,
						percent);
		} else {
			lv_snprintf(line, sizeof(line), t("%d/%d %s", "%d/%d %s"), step, job_steps, job_step_id);
		}
		set_status(list_status, line);
	} else if (progress.total > 0) {
		long percent = progress.done * 100 / progress.total;
		if (percent > 100) {
			percent = 100;
		}
		lv_snprintf(line, sizeof(line), "%s %ld%%",
					job_kind == JOB_REFRESH ? t("asking GitHub…", "запрашиваю GitHub…")
											: t("downloading", "скачиваю"),
					percent);
		set_status(job_kind == JOB_REFRESH ? list_status : detail_status, line);
	} else if (job_kind == JOB_REFRESH) {
		set_refresh_status(job_reading ? t("reading the releases…", "читаю релизы…")
									   : t("asking GitHub…", "запрашиваю GitHub…"));
	} else {
		set_status(detail_status, t("working…", "работаю…"));
	}

	bool done;
	pthread_mutex_lock(&job_lock);
	done = job_done;
	pthread_mutex_unlock(&job_lock);
	if (!done) {
		return;
	}

	pthread_mutex_lock(&job_lock);
	job_kind_t kind = job_kind;
	bool ok = job_ok;
	char error[AS_ERROR_MAX];
	lv_snprintf(error, sizeof(error), "%s", job_error);
	char version[AS_VERSION_MAX];
	lv_snprintf(version, sizeof(version), "%s", job_version);
	refresh_result_t *fresh = job_refresh;
	job_refresh = NULL;
	update_result_set_t *updates = job_updates;
	job_updates = NULL;
	as_release_t release = job_release;
	job_kind = JOB_NONE;
	pthread_mutex_unlock(&job_lock);

	busy = false;
	if (poll_timer) {
		lv_timer_pause(poll_timer);
	}

	if (kind == JOB_REFRESH) {
		// A failed fetch keeps the cached list: a store that shows nothing
		// when GitHub is unreachable is worse than one that shows yesterday's
		// release.
		if (fresh && ok) {
			model.release_count = 0;
			for (int i = 0; i < fresh->count && model.release_count < AS_MAX; i++) {
				model.release[model.release_count++] = fresh->rel[i];
			}
			as_cache_save(&model);
		}
		free(fresh);
		fresh = NULL;
		refreshed_this_session = true;
		build_list();
		set_refresh_status(ok ? "" : error);
		if (!ok && error[0]) {
			toast_error(error);
		}
		return;
	}

	if (kind == JOB_UPDATE_ALL) {
		int updated = 0;
		int failed = 0;
		char first_error[AS_ERROR_MAX] = "";
		if (updates) {
			for (int i = 0; i < updates->count; i++) {
				update_result_t *r = &updates->item[i];
				if (!r->ok) {
					failed++;
					if (!first_error[0]) {
						lv_snprintf(first_error, sizeof(first_error), "%s", r->error);
					}
					continue;
				}
				const as_release_t *rel = as_release_find(&model, r->id);
				if (rel) {
					as_record_t rec;
					memset(&rec, 0, sizeof(rec));
					lv_snprintf(rec.id, sizeof(rec.id), "%s", rel->id);
					lv_snprintf(rec.repo, sizeof(rec.repo), "%s", rel->repo);
					lv_snprintf(rec.tag, sizeof(rec.tag), "%s", rel->tag);
					lv_snprintf(rec.asset, sizeof(rec.asset), "%s", rel->asset);
					rec.size = rel->size;
					lv_snprintf(rec.digest, sizeof(rec.digest), "%s", rel->digest);
					lv_snprintf(rec.version, sizeof(rec.version), "%s", r->version);
					as_record_put(&model, &rec);
				}
				updated++;
			}
			free(updates);
			updates = NULL;
		}
		as_scan_installed(&model);
		build_list();
		char summary[160];
		if (failed > 0) {
			lv_snprintf(summary, sizeof(summary), t("%d updated, %d failed: %s", "%d обновлено, %d с ошибкой: %s"),
						updated, failed, first_error);
			set_status(list_status, summary);
			toast_error(first_error[0] ? first_error : t("some updates failed", "часть обновлений не удалась"));
		} else {
			lv_snprintf(summary, sizeof(summary), t("%d updated", "%d обновлено"), updated);
			set_status(list_status, summary);
			if (updated > 0) {
				toast_success(summary);
			}
		}
		return;
	}

	if (kind == JOB_INSTALL) {
		if (ok) {
			as_record_t rec;
			memset(&rec, 0, sizeof(rec));
			lv_snprintf(rec.id, sizeof(rec.id), "%s", release.id);
			lv_snprintf(rec.repo, sizeof(rec.repo), "%s", release.repo);
			lv_snprintf(rec.tag, sizeof(rec.tag), "%s", release.tag);
			lv_snprintf(rec.asset, sizeof(rec.asset), "%s", release.asset);
			rec.size = release.size;
			lv_snprintf(rec.digest, sizeof(rec.digest), "%s", release.digest);
			lv_snprintf(rec.version, sizeof(rec.version), "%s", version);
			as_record_put(&model, &rec);
			as_scan_installed(&model);
			if (addon_id && strcmp(release.id, addon_id) == 0) {
				toast_success(t("store updated — reopen the tile to run it",
								"магазин обновлён — откройте плитку заново"));
			} else {
				toast_success(t("installed — reinsert the card to get its tile",
								"установлено — выньте и вставьте карту, чтобы появилась плитка"));
			}
			build_list();
			open_detail(release.id);
		} else {
			set_status(detail_status, error);
			toast_error(error);
		}
		return;
	}

	if (kind == JOB_README) {
		if (ok) {
			open_readme_page(release.id);
		} else {
			set_status(detail_status, error);
			toast_error(error);
		}
	}
}

// ---------------------------------------------------------------------------
// The addon
// ---------------------------------------------------------------------------

static lv_obj_t *store_init(const sonix_addon_host_t *h) {
	// A reloaded library starts from the same statics it left: init() has to be
	// able to run as on a fresh mapping (addon.h).
	pthread_mutex_lock(&job_lock);
	stopping = false;
	workers_live = 0;
	job_kind = JOB_NONE;
	job_done = false;
	job_ok = false;
	job_refresh = NULL;
	job_updates = NULL;
	job_step = 0;
	job_steps = 0;
	job_step_id[0] = '\0';
	busy = false;
	refreshed_this_session = false;
	memset(&progress, 0, sizeof(progress));
	pthread_mutex_unlock(&job_lock);

	host = h;
	cfg = h->cfg;
	addon_id = h->id;
	query[0] = '\0';
	update_plan_count = 0;
	entry_mode = ENTRY_SEARCH;

	char cache[512];
	addon_cache_dir(h, cache, sizeof(cache));
	as_model_init(&model, h->dir, h->sd_root, cache);
	as_repos_load(&model);
	as_cache_load(&model);
	as_records_load(&model);
	as_local_load(&model);
	as_scan_installed(&model);
	printf("addonstore: %d repo(s), %d installed, %d release entries, %d fingerprint(s), cache %s\n",
		   model.repo_count, model.installed_count, model.release_count, model.local_count, cache);

	list_screen = lv_obj_create(NULL);
	sonix_addon_own_screen(addon_id, list_screen);
	list_container = scrolling_page(list_screen, t("Store", "Магазин"), NULL);

	detail_screen = lv_obj_create(NULL);
	sonix_addon_own_screen(addon_id, detail_screen);
	lv_obj_add_style(detail_screen, &theme_style_screen, 0);
	detail_title = settingsrow_title(detail_screen, cfg, "Addon");
	detail_container = lv_obj_create(detail_screen);
	lv_obj_set_size(detail_container, lv_pct(100), cfg->screen_height - settingsrow_content_top(cfg));
	lv_obj_align(detail_container, LV_ALIGN_TOP_LEFT, 0, settingsrow_content_top(cfg));
	lv_obj_set_style_bg_opa(detail_container, 0, 0);
	lv_obj_set_style_border_width(detail_container, 0, 0);
	lv_obj_set_style_radius(detail_container, 0, 0);
	lv_obj_set_style_pad_hor(detail_container, cfg->padding, 0);
	lv_obj_set_style_pad_ver(detail_container, cfg->padding, 0);
	lv_obj_set_style_pad_gap(detail_container, SC(8), 0);
	lv_obj_set_flex_flow(detail_container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(detail_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scroll_dir(detail_container, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(detail_container, LV_SCROLLBAR_MODE_ACTIVE);
	lv_obj_set_scroll_elastic(detail_container, false);
	switcher_attach_back_gesture(detail_screen);

	readme_screen = lv_obj_create(NULL);
	sonix_addon_own_screen(addon_id, readme_screen);
	readme_container = scrolling_page(readme_screen, "README", &readme_title);

	repos_screen = lv_obj_create(NULL);
	sonix_addon_own_screen(addon_id, repos_screen);
	repos_container = scrolling_page(repos_screen, t("Repositories", "Репозитории"), NULL);
	build_repos();

	// One field, one keyboard: "Search" and "Add repository" share them. The
	// keyboard is the largest object in this addon, and a second one would cost
	// more RAM than this device has spare.
	entry_screen = lv_obj_create(NULL);
	sonix_addon_own_screen(addon_id, entry_screen);
	lv_obj_add_style(entry_screen, &theme_style_screen, 0);
	entry_title = settingsrow_title(entry_screen, cfg, t("Search", "Поиск"));
	int entry_top = settingsrow_content_top(cfg);
	entry_field = lv_textarea_create(entry_screen);
	lv_textarea_set_one_line(entry_field, true);
	lv_textarea_set_placeholder_text(entry_field, t("name or id", "имя или id"));
	lv_obj_set_size(entry_field, cfg->screen_width - 2 * cfg->padding, SC(64));
	lv_obj_align(entry_field, LV_ALIGN_TOP_LEFT, cfg->padding, entry_top);
	lv_obj_set_scrollbar_mode(entry_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_style(entry_field, &theme_style_card, 0);
	lv_obj_set_style_radius(entry_field, SC(12), 0);
	lv_obj_set_style_border_width(entry_field, 0, 0);
	lv_obj_set_style_shadow_width(entry_field, 0, 0);
	lv_obj_set_style_pad_all(entry_field, SC(14), 0);
	lv_obj_set_style_text_font(entry_field, &font_ui_24, 0);
	keyboard_style_caret(entry_field);
	entry_kb = keyboard_create(entry_screen, cfg->screen_width, SC(316), entry_field, NULL, "ok", entry_accept_cb,
							   NULL);
	switcher_attach_back_gesture(entry_screen);

	build_list();
	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	lv_timer_pause(poll_timer);
	power_pause_in_standby(poll_timer);

	lv_obj_add_event_cb(list_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	return list_screen;
}

static bool store_stop(void) {
	stopping = true;
	progress.stop = true;
	// The download, if one is running, is parked in a recv() with a 30 s
	// timeout: wake it, or this wait spends STOP_WAIT_MS and then says the
	// library is not quiet -- which on the leave path would keep it loaded for
	// the rest of the run. The refresh/scan jobs use http_get_ex() and cannot be
	// woken; those are the ones the bounded wait is still for.
	as_cancel();

	pthread_mutex_lock(&job_lock);
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += STOP_WAIT_MS / 1000;
	deadline.tv_nsec += (long)(STOP_WAIT_MS % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	while (workers_live > 0) {
		if (pthread_cond_timedwait(&job_cond, &job_lock, &deadline) != 0) {
			break;
		}
	}
	bool quiet = workers_live == 0;
	refresh_result_t *fresh = job_refresh;
	job_refresh = NULL;
	update_result_set_t *updates = job_updates;
	job_updates = NULL;
	pthread_mutex_unlock(&job_lock);
	free(fresh);
	free(updates);

	if (poll_timer) {
		power_forget_timer(poll_timer);
		lv_timer_delete(poll_timer);
		poll_timer = NULL;
	}
	if (list_screen) {
		switcher_forget_screen(list_screen);
	}
	if (detail_screen) {
		switcher_forget_screen(detail_screen);
	}
	if (readme_screen) {
		switcher_forget_screen(readme_screen);
	}
	if (repos_screen) {
		switcher_forget_screen(repos_screen);
	}
	if (entry_screen) {
		switcher_forget_screen(entry_screen);
	}
	list_screen = NULL;
	list_container = NULL;
	list_status = NULL;
	detail_screen = NULL;
	detail_title = NULL;
	detail_container = NULL;
	detail_status = NULL;
	readme_screen = NULL;
	readme_container = NULL;
	readme_title = NULL;
	repos_screen = NULL;
	repos_container = NULL;
	repos_status = NULL;
	entry_screen = NULL;
	entry_field = NULL;
	entry_title = NULL;
	entry_kb = NULL;
	host = NULL;
	cfg = NULL;
	addon_id = NULL;
	detail_id[0] = '\0';
	query[0] = '\0';
	update_plan_count = 0;
	repo_row_count = 0;
	row_count = 0;
	detail_id[0] = '\0';
	busy = false;
	return quiet;
}

static const sonix_addon_t store = {
	.api = SONIX_ADDON_API,
	.title = "Store",
	.init = store_init,
};

static const sonix_addon_stop_t store_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = store_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &store : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &store_lifecycle : NULL;
}
