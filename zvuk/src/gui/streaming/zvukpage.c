#include "src/gui/streaming/zvukpage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/listrow.h"

#include "src/addon/addon.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/listart.h"
#include "src/system/core/lang.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/podcaststate.h"
#include "src/system/streaming/streamerr.h"
#include "src/system/streaming/zvuk.h"
#include "src/system/streaming/zvukcompat.h"
#include "src/system/streaming/zvukdl.h"
#include "src/system/streaming/zvuklist.h"
#include "src/system/streaming/zvukplay.h"
#include "src/system/streaming/zvukservice.h"
#include "src/system/streaming/zvukstop.h"
#include "src/system/streaming/zvuksync.h"

// ---------------------------------------------------------------------------
// Zvuk, the page.
//
// Ported from rekkod's zvukpage.c, which was written for Graphite 2: its rows
// were sx_row_*, its cards sx_card_*, its filters sx_pill and its per-row menu
// an sx_sheet. None of that is carried (docs/ADDONS.md, "Porting a service
// page"): the page's own rows are settingsrow_*, a list's rows are a button the
// page builds itself, the sleeves on them come from addons/common/listrow.h
// (which is listart's downloader with the per-row bookkeeping), and the menu is
// this player's popover.
//
// What is kept from that page, because it is the service rather than the
// drawing: the job worker and its one-slot queue, the result array the UI
// thread reads, the list history behind the chevron, the pages of a long list
// through zvuklist, the likes through zvuksync, the saved tracks through zvukdl
// and playback through zvukplay_list.
//
// What is not here, and why:
//
//   * the queue actions ("play next", "add to the end") -- svcui_queue and
//     playlist_queue_where_t belong to the fork's player; this one has no
//     public "add these tracks to the queue" path for a service;
//   * playbackplace -- there is no such module here, so the list is not
//     reopened where a track left off;
//   * cardimport -- the fork's scanner for files at the card's root. The token
//     file is read here, by the page, as the account screen did;
//   * streamerror -- the module is not carried either: a failure is one
//     sentence in the list's empty label and a toast;
//   * the album and artist bars -- the same actions are in the per-row menu.
//
// Threads: everything that talks to the network runs on the worker below and
// hands its answer back with gui_post(), which runs on the UI thread. init()
// runs once; what depends on the card or the session is redone on every
// SCREEN_LOADED of the home screen.
// ---------------------------------------------------------------------------

#define MAX_ROWS 150
#define BROWSE_BOOKS_PAGE 20
#define ARTIST_TRACKS 20

// One row of a list, and the gap the container puts between rows.
#define LIST_ROW_H SC(88)
#define LIST_ROW_GAP SC(10)
#define LIST_THUMB SC(60)

// The label shown on a row that has no picture: an artist, a playlist.
#define LETTER_BOX LIST_THUMB

typedef enum { ROW_TRACK, ROW_ALBUM, ROW_ARTIST, ROW_PLAYLIST, ROW_BOOK, ROW_PODCAST, ROW_MORE } row_kind_t;

typedef struct {
	row_kind_t kind;
	union {
		zvuk_track_t track;
		zvuk_album_t album;
		zvuk_artist_t artist;
		zvuk_playlist_t playlist;
	} u;
} row_t;

typedef enum {
	JOB_NONE = 0,
	// lists
	JOB_LIKED,
	JOB_ALBUMS,
	JOB_ARTISTS,
	JOB_PLAYLISTS,
	JOB_PLAYLIST,
	JOB_ALBUM,
	JOB_BOOK,
	JOB_PODCAST,
	JOB_ARTIST,
	JOB_SEARCH,		  // `n`: the kind (zvuk_search_kind_t)
	JOB_BROWSE_BOOKS, // `n`: offset, `b`: children or empty
	JOB_BROWSE_PICKS,
	// one page of the list on screen
	JOB_PAGE,
	// in order, none dropped: the account's profile, the connection check
	JOB_PROFILE,
	JOB_CHECK,
	// built on the GUI thread from the index of saved tracks
	JOB_SAVED,
	JOB_SAVED_ALBUM,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char a[ZVUK_TITLE_MAX];
	char b[ZVUK_ID_MAX];
	int n;
	unsigned generation;
	zvuklist_t *list;
} job_t;

typedef struct {
	job_kind_t kind;
	unsigned generation;
	int page;
	int n;
	char id[ZVUK_ID_MAX];
	char error[512];
	stream_err_t error_class;
	zvuklist_t *list;
	int count;
	zvuk_spoken_t spoken;
	row_t rows[];
} job_result_t;

lv_obj_t *zvukpage_screen;

static lv_obj_t *list_screen, *search_screen;
static gui_config_t *page_cfg;
static char card_root[400]; // the card: the covers' shared cache, the saved tracks
static char addon_dir[400]; // this addon's folder on the card: the token, beside the settings

// The id the host gave this addon, kept because every screen it builds has to
// be registered with the shell as it is made, and the shell finds the addon by
// that name (sonix_addon_own_screen, src/addon/addon.h).
static char addon_id[ADDON_ID_MAX];

// The covers of a list. Only its address matters (listart.h gives `owner` the
// same meaning); one page owns the slots at a time.
static const char zvuk_art_owner;

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static job_t pending, page_pending;
#define EVENTS_MAX 8
static job_t events[EVENTS_MAX];
static int events_count;
static bool worker_started;
static unsigned list_generation;

// The worker and the sign-in it may have started, the flag that tells them to
// leave, and whether this page's two theme hooks are in the player's table:
// see zvukstop.h and zvukpage_stop().
static zvuk_stop_t threads = ZVUK_STOP_INIT;
static bool stopping;
static bool theme_registered;

// Worker side: the answer being built.
static row_t *result;
static int result_count;
static zvuklist_t *result_list;
static char result_error[512];
static stream_err_t result_class;
static zvuk_spoken_t result_spoken;

static void job_done_async(void *user);

static bool is_event(job_kind_t kind) { return kind == JOB_PROFILE || kind == JOB_CHECK; }

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	if (stopping) {
		// The worker is on its way out, and a job accepted now is one that
		// would never run: the page's list ref goes with it.
		pthread_mutex_unlock(&job_lock);
		if (job->kind == JOB_PAGE) {
			zvuklist_free(job->list);
		}
		return;
	}
	if (is_event(job->kind)) {
		if (events_count < EVENTS_MAX) {
			events[events_count++] = *job;
		}
	} else if (job->kind == JOB_PAGE) {
		zvuklist_free(page_pending.list);
		page_pending = *job;
	} else {
		pending = *job;
	}
	pthread_cond_signal(&job_cond);
	pthread_mutex_unlock(&job_lock);
}

static void add_id_cb(const char *id, void *user) { zvuklist_add_id(user, id); }

static int list_from_tracks(const zvuk_track_t *tracks, int got) {
	if (got < 0) {
		return got;
	}
	result_list = zvuklist_new();
	for (int i = 0; result_list && i < got; i++) {
		zvuklist_add_track(result_list, &tracks[i]);
	}
	return result_list ? got : -1;
}

static int list_from_ids(int got, const char *what) {
	if (got < 0 || !result_list) {
		zvuklist_free(result_list);
		result_list = NULL;
		return -1;
	}
	if (got > 0 && zvuklist_fetch_page(result_list, 0) < 0) {
		fprintf(stderr, "zvuk: %s: the first page did not come (%s)\n", what, zvuk_last_error());
	}
	printf("zvuk: %s: %d tracks, the list holds %zu bytes\n", what, got, zvuklist_memory(result_list));
	return got;
}

static void add_row(row_kind_t kind, const void *what) {
	if (result_count >= MAX_ROWS) {
		return;
	}
	row_t *r = &result[result_count++];
	r->kind = kind;
	switch (kind) {
	case ROW_TRACK:
		r->u.track = *(const zvuk_track_t *)what;
		break;
	case ROW_ALBUM:
	case ROW_BOOK:
	case ROW_PODCAST:
		r->u.album = *(const zvuk_album_t *)what;
		break;
	case ROW_ARTIST:
		r->u.artist = *(const zvuk_artist_t *)what;
		break;
	case ROW_PLAYLIST:
		r->u.playlist = *(const zvuk_playlist_t *)what;
		break;
	case ROW_MORE:
		memset(&r->u, 0, sizeof(r->u));
		break;
	}
}

static void run_list_job(const job_t *job) {
	if (job->kind == JOB_BOOK || job->kind == JOB_PODCAST) {
		int got = -1;
		if (job->kind == JOB_BOOK) {
			zvuk_spoken_part_t *parts = calloc(ZVUK_SPOKEN_MAX_PARTS, sizeof(*parts));
			got = parts ? zvuk_book_parts(job->a, parts, ZVUK_SPOKEN_MAX_PARTS, &result_spoken) : -1;
			if (got >= 0) {
				result_list = zvuklist_new();
				for (int i = 0; result_list && i < got; i++) {
					if (!zvuklist_add_track(result_list, &parts[i].track)) {
						got = -1;
						break;
					}
				}
			}
			free(parts);
		} else {
			char(*ids)[ZVUK_ID_MAX] = NULL;
			got = zvuk_podcast_ids(job->a, &ids, &result_spoken);
			if (got >= 0) {
				result_list = zvuklist_new_episodes();
				for (int i = 0; result_list && i < got; i++) {
					if (!zvuklist_add_id(result_list, ids[i])) {
						got = -1;
						break;
					}
				}
			}
			free(ids);
			if (got >= 0) {
				got = list_from_ids(got, "podcast episodes");
			}
		}
		if (got < 0 || !result_list) {
			zvuklist_free(result_list);
			result_list = NULL;
			result_class = streamerr_describe("zvuk", zvuk_last_error(), result_error, sizeof(result_error));
		}
		return;
	}

	void *tmp = calloc(MAX_ROWS, sizeof(zvuk_track_t));
	if (!tmp) {
		snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
		return;
	}
	zvuk_track_t *tracks = tmp;
	int got = -1;
	switch (job->kind) {
	case JOB_LIKED:
		result_list = zvuklist_new();
		got = list_from_ids(result_list ? zvuk_liked_ids(add_id_cb, result_list) : -1, "liked tracks");
		break;
	case JOB_PLAYLIST:
		result_list = zvuklist_new();
		got = list_from_ids(result_list ? zvuk_playlist_ids(job->a, add_id_cb, result_list) : -1, "playlist");
		break;
	case JOB_ALBUM:
		if (!zvuk_liked_known(ZVUK_ITEM_RELEASE)) {
			zvuk_collection_refresh();
		}
		got = list_from_tracks(tracks, zvuk_album_tracks(job->a, tracks, MAX_ROWS, NULL));
		break;
	case JOB_ALBUMS: {
		zvuk_album_t *a = tmp;
		got = zvuk_liked_albums(a, MAX_ROWS);
		for (int i = 0; i < got; i++) {
			add_row(ROW_ALBUM, &a[i]);
		}
		break;
	}
	case JOB_ARTISTS: {
		zvuk_artist_t *a = tmp;
		got = zvuk_liked_artists(a, MAX_ROWS);
		for (int i = 0; i < got; i++) {
			add_row(ROW_ARTIST, &a[i]);
		}
		break;
	}
	case JOB_PLAYLISTS: {
		zvuk_playlist_t *p = tmp;
		got = zvuk_playlists(p, MAX_ROWS);
		for (int i = 0; i < got; i++) {
			add_row(ROW_PLAYLIST, &p[i]);
		}
		break;
	}
	case JOB_BROWSE_PICKS: {
		zvuk_playlist_t *p = tmp;
		got = zvuk_recommended_playlists(p, 50);
		for (int i = 0; i < got; i++) {
			add_row(ROW_PLAYLIST, &p[i]);
		}
		break;
	}
	case JOB_BROWSE_BOOKS: {
		zvuk_album_t *books = tmp;
		bool more = false;
		got = zvuk_recommended_books(!strcmp(job->b, "children"), job->n, books, BROWSE_BOOKS_PAGE, &more);
		for (int i = 0; i < got; i++) {
			add_row(ROW_BOOK, &books[i]);
		}
		if (got >= 0 && more) {
			add_row(ROW_MORE, NULL);
		}
		break;
	}
	case JOB_ARTIST: {
		if (!zvuk_liked_known(ZVUK_ITEM_ARTIST)) {
			zvuk_collection_refresh();
		}
		int nt = 0, na = 0;
		zvuk_album_t *albums = calloc(MAX_ROWS, sizeof(*albums));
		got = albums ? zvuk_artist_page(job->a, tracks, ARTIST_TRACKS, &nt, albums, MAX_ROWS - ARTIST_TRACKS, &na) : -1;
		for (int i = 0; i < nt; i++) {
			add_row(ROW_TRACK, &tracks[i]);
		}
		for (int i = 0; i < na; i++) {
			add_row(ROW_ALBUM, &albums[i]);
		}
		free(albums);
		break;
	}
	case JOB_SEARCH: {
		zvuk_search_kind_t kind = (zvuk_search_kind_t)job->n;
		got = zvuk_search(job->a, kind, tmp, kind == ZVUK_SEARCH_TRACKS ? 60 : 40);
		static const row_kind_t ROWS[ZVUK_SEARCH_KINDS] = {ROW_TRACK, ROW_ALBUM, ROW_ARTIST,
														   ROW_PLAYLIST, ROW_BOOK, ROW_PODCAST};
		static const size_t SIZES[ZVUK_SEARCH_KINDS] = {sizeof(zvuk_track_t), sizeof(zvuk_album_t),
														sizeof(zvuk_artist_t), sizeof(zvuk_playlist_t),
														sizeof(zvuk_album_t), sizeof(zvuk_album_t)};
		for (int i = 0; i < got && (unsigned)kind < ZVUK_SEARCH_KINDS; i++) {
			add_row(ROWS[kind], (const char *)tmp + (size_t)i * SIZES[kind]);
		}
		break;
	}
	default:
		break;
	}
	if (got < 0) {
		zvuklist_free(result_list);
		result_list = NULL;
		result_count = 0;
		result_class = streamerr_describe("zvuk", zvuk_last_error(), result_error, sizeof(result_error));
	}
	free(tmp);
}

static job_result_t *job_result_new(const job_t *job) {
	int count = result_count;
	job_result_t *r = malloc(sizeof(*r) + (size_t)count * sizeof(row_t));
	if (!r) {
		zvuklist_free(result_list);
		result_list = NULL;
		return NULL;
	}
	memset(r, 0, sizeof(*r));
	r->kind = job->kind;
	r->generation = job->generation;
	r->page = job->n;
	r->n = job->n;
	snprintf(r->id, sizeof(r->id), "%.*s", ZVUK_ID_MAX - 1, job->a);
	snprintf(r->error, sizeof(r->error), "%s", result_error);
	r->error_class = result_class;
	r->list = result_list;
	result_list = NULL;
	r->count = count;
	r->spoken = result_spoken;
	if (count > 0) {
		memcpy(r->rows, result, (size_t)count * sizeof(row_t));
	}
	return r;
}

static void free_result(job_result_t *r) {
	if (r) {
		zvuklist_free(r->list);
		free(r);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	// The count was raised by start_worker(), under the lock that decides
	// whether a worker may start at all: a thread that has been created but has
	// not reached its first line is still a thread stop() would dlclose() from
	// under.
	zvuk_http_set_thread_timeout_cap(STREAM_UI_TIMEOUT_SECS);
	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (pending.kind == JOB_NONE && page_pending.kind == JOB_NONE && events_count == 0 && !stopping) {
			pthread_cond_wait(&job_cond, &job_lock);
		}
		if (stopping) {
			pthread_mutex_unlock(&job_lock);
			break;
		}
		job_t job;
		if (pending.kind != JOB_NONE) {
			job = pending;
			pending.kind = JOB_NONE;
		} else if (page_pending.kind != JOB_NONE) {
			job = page_pending;
			page_pending.kind = JOB_NONE;
			page_pending.list = NULL;
		} else {
			job = events[0];
			memmove(events, events + 1, (size_t)(--events_count) * sizeof(events[0]));
		}
		pthread_mutex_unlock(&job_lock);

		result_error[0] = '\0';
		result_class = STREAM_ERR_NONE;
		result_count = 0;
		result_list = NULL;
		memset(&result_spoken, 0, sizeof(result_spoken));
		streamerr_begin();
		switch (job.kind) {
		case JOB_PAGE:
			if (zvuklist_fetch_page(job.list, job.n) < 0) {
				result_class = streamerr_describe("zvuk", zvuk_last_error(), result_error, sizeof(result_error));
			}
			result_list = job.list;
			job.list = NULL;
			break;
		case JOB_PROFILE:
			zvuk_refresh_profile();
			break;
		case JOB_CHECK:
			zvuk_check_connection(result_error, sizeof(result_error));
			break;
		default:
			run_list_job(&job);
			break;
		}
		job_result_t *r = job_result_new(&job);
		if (r && !gui_post(job_done_async, r)) {
			free_result(r);
		}
	}
	zvuk_stop_leave(&threads);
	return NULL;
}

static void start_worker(void) {
	if (worker_started || stopping) {
		return;
	}
	if (!result) {
		result = calloc(MAX_ROWS, sizeof(*result));
	}
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	// Counted before it exists and uncounted when it never started: a thread
	// that is created and not yet running must not slip past the wait in
	// zvukpage_stop().
	zvuk_stop_enter(&threads);
	worker_started = result && pthread_create(&th, &attr, worker_main, NULL) == 0;
	if (!worker_started) {
		zvuk_stop_leave(&threads);
	}
	pthread_attr_destroy(&attr);
}

static void run(job_kind_t kind, const char *a, const char *b, int n) {
	start_worker();
	job_t job = {.kind = kind, .n = n, .generation = list_generation};
	snprintf(job.a, sizeof(job.a), "%s", a ? a : "");
	snprintf(job.b, sizeof(job.b), "%s", b ? b : "");
	submit(&job);
}

// ---------------------------------------------------------------------------
// what the list is showing
// ---------------------------------------------------------------------------

static lv_obj_t *list_container, *list_title, *list_empty, *list_spin;
static job_kind_t list_kind;
static job_result_t *shown;
static int page_asked = -1, page_failed = -1;
static time_t page_failed_at;
static char playing_id[ZVUK_ID_MAX];

static void rebuild_home(void);
static void render_list(void);
static void row_clicked_cb(lv_event_t *e);
static void row_menu_cb(lv_event_t *e);
static void list_scrolled_cb(lv_event_t *e);

static int view_count(void) {
	if (!shown) {
		return 0;
	}
	return shown->list ? zvuklist_count(shown->list) : shown->count;
}

// One entry of what is on screen. False while the entry's details are still on
// the way: `out` then carries the id and nothing else.
static bool view_row(int index, row_t *out) {
	memset(out, 0, sizeof(*out));
	out->kind = ROW_TRACK;
	if (!shown || index < 0 || index >= view_count()) {
		return false;
	}
	if (!shown->list) {
		*out = shown->rows[index];
		return true;
	}
	if (zvuklist_track(shown->list, index, &out->u.track)) {
		return true;
	}
	zvuklist_id(shown->list, index, out->u.track.id, sizeof(out->u.track.id));
	return false;
}

static bool row_saved(const row_t *r) {
	if (r->kind == ROW_TRACK) {
		if (r->u.track.content == ZVUK_CONTENT_CHAPTER) {
			char path[512];
			return zvukdl_local_book(r->u.track.album_id, path, sizeof(path));
		}
		if (r->u.track.content == ZVUK_CONTENT_EPISODE) {
			char path[512];
			return zvukdl_local_episode(&r->u.track, path, sizeof(path));
		}
		return r->u.track.id[0] && zvukdl_saved(r->u.track.id);
	}
	if (r->kind == ROW_ALBUM && r->u.album.id[0] && r->u.album.track_count > 0) {
		return zvukdl_album_saved_count(r->u.album.id) >= r->u.album.track_count;
	}
	if (r->kind == ROW_BOOK && r->u.album.id[0]) {
		char path[512];
		return zvukdl_local_book(r->u.album.id, path, sizeof(path));
	}
	return false;
}

static void duration_text(int seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

static void first_char(const char *text, char *out, size_t size) {
	unsigned char c = (unsigned char)(text ? text[0] : 0);
	size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
	if (!c || n >= size || strlen(text) < n) {
		snprintf(out, size, "#");
		return;
	}
	memcpy(out, text, n);
	out[n] = '\0';
	if (n == 1 && out[0] >= 'a' && out[0] <= 'z') {
		out[0] = (char)(out[0] - 'a' + 'A');
	}
}

// Name and detail of one row, as the list shows them.
static void row_texts(const row_t *r, char *name, size_t ns, char *detail, size_t ds) {
	switch (r->kind) {
	case ROW_MORE:
		snprintf(name, ns, "More");
		detail[0] = '\0';
		break;
	case ROW_TRACK: {
		snprintf(name, ns, "%s", r->u.track.title);
		char length[16];
		duration_text(r->u.track.duration, length, sizeof(length));
		if (r->u.track.content == ZVUK_CONTENT_EPISODE) {
			char date[16];
			snprintf(date, sizeof(date), "%.10s", r->u.track.published_at);
			snprintf(detail, ds, "%s%s%s%s%s", date, date[0] ? " \xC2\xB7 " : "", r->u.track.artist,
					 length[0] ? " \xC2\xB7 " : "", length);
		} else {
			snprintf(detail, ds, "%s%s%s", r->u.track.artist, length[0] ? " \xC2\xB7 " : "", length);
		}
		if (row_saved(r)) {
			size_t used = strlen(detail);
			snprintf(detail + used, ds > used ? ds - used : 0, "%sSaved", used ? " \xC2\xB7 " : "");
		}
		break;
	}
	case ROW_ALBUM:
	case ROW_BOOK:
	case ROW_PODCAST:
		snprintf(name, ns, "%s", r->u.album.title);
		if (r->u.album.year) {
			snprintf(detail, ds, "%s \xC2\xB7 %d", r->u.album.artist, r->u.album.year);
		} else {
			snprintf(detail, ds, "%s", r->u.album.artist);
		}
		if (r->kind == ROW_BOOK || r->kind == ROW_PODCAST) {
			size_t used = strlen(detail);
			snprintf(detail + used, ds > used ? ds - used : 0, "%s%s", used ? " \xC2\xB7 " : "",
					 r->kind == ROW_BOOK ? "Audiobook" : "Podcast");
		}
		break;
	case ROW_ARTIST:
		snprintf(name, ns, "%s", r->u.artist.name);
		snprintf(detail, ds, "Artist");
		break;
	case ROW_PLAYLIST:
		snprintf(name, ns, "%s", r->u.playlist.title);
		if (r->u.playlist.track_count > 0) {
			snprintf(detail, ds, "%d tracks", r->u.playlist.track_count);
		} else {
			snprintf(detail, ds, "Playlist");
		}
		break;
	}
}

static bool playing_update(void) {
	char id[ZVUK_ID_MAX];
	zvukplay_playing_id(id, sizeof(id));
	if (strcmp(id, playing_id) == 0) {
		return false;
	}
	snprintf(playing_id, sizeof(playing_id), "%s", id);
	return true;
}

// ---------------------------------------------------------------------------
// a row
// ---------------------------------------------------------------------------

// Whether the row has anything in its menu: a like, a download.
static bool row_menu_available(const row_t *r) {
	if (!zvuk_logged_in()) {
		return false;
	}
	switch (r->kind) {
	case ROW_TRACK:
		return r->u.track.id[0] != '\0';
	case ROW_ALBUM:
	case ROW_ARTIST:
	case ROW_PLAYLIST:
	case ROW_BOOK:
		return true;
	default:
		return false;
	}
}

// The sleeve: an lv_image the page lays out, whose source listrow sets when the
// picture arrives (listrow.h). Before that it is a quiet rounded square, which
// is also what it is when [streaming] list_art is off.
static lv_obj_t *make_thumb(lv_obj_t *row, int index, const char *cover) {
	lv_obj_t *thumb = lv_image_create(row);
	lv_obj_set_size(thumb, LIST_THUMB, LIST_THUMB);
	lv_obj_set_style_bg_color(thumb, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(thumb, SC(8), 0);
	lv_obj_set_style_clip_corner(thumb, true, 0);
	lv_obj_set_event_bubble(thumb, true);
	char url[ZVUK_URL_MAX];
	zvuk_cover_url(cover, 100, url, sizeof(url));
	listrow_bind((void *)&zvuk_art_owner, thumb, index, url);
	return thumb;
}

// A letter where there is no picture: an artist, a playlist. The same idea as
// the fork's sx_row_set_letter, drawn with this player's objects.
static lv_obj_t *make_letter(lv_obj_t *row, const char *text) {
	lv_obj_t *box = lv_obj_create(row);
	lv_obj_set_size(box, LETTER_BOX, LETTER_BOX);
	lv_obj_set_style_bg_color(box, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_radius(box, SC(8), 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_event_bubble(box, true);
	lv_obj_t *label = lv_label_create(box);
	char letter[8];
	first_char(text, letter, sizeof(letter));
	lv_label_set_text(label, letter);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);
	return box;
}

// The track's place on the release, where the list is one album's tracks.
static lv_obj_t *make_number(lv_obj_t *row, int number) {
	lv_obj_t *box = lv_obj_create(row);
	lv_obj_set_size(box, LETTER_BOX, LETTER_BOX);
	lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_event_bubble(box, true);
	lv_obj_t *label = lv_label_create(box);
	lv_label_set_text_fmt(label, "%d", number);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);
	return box;
}

static bool album_like_list(void) {
	return list_kind == JOB_ALBUM || list_kind == JOB_SAVED_ALBUM || list_kind == JOB_BOOK;
}

static void make_row(int index) {
	row_t r;
	bool have = view_row(index, &r);

	lv_obj_t *row = lv_btn_create(list_container);
	lv_obj_set_size(row, lv_pct(100), LIST_ROW_H);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, SC(12), 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, SC(14), 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_style_pad_column(row, SC(10), 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// Gestures (back, and pulling in the player sheet) start on rows too, and a
	// button keeps the press to itself: without this neither drag works here.
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	char name[ZVUK_TITLE_MAX], detail[ZVUK_TITLE_MAX + 48];
	if (!have) {
		// The list is here, the details are not yet: the row says so instead of
		// vanishing, so the list does not jump when the page arrives.
		snprintf(name, sizeof(name), "%s", tr("loading"));
		detail[0] = '\0';
	} else {
		row_texts(&r, name, sizeof(name), detail, sizeof(detail));
	}

	// The lead: a sleeve, a letter, or the track's number.
	if (have) {
		switch (r.kind) {
		case ROW_TRACK:
			if (album_like_list() && r.u.track.track_number > 0) {
				make_number(row, r.u.track.track_number);
			} else if (r.u.track.cover[0]) {
				make_thumb(row, index, r.u.track.cover);
			}
			break;
		case ROW_ALBUM:
		case ROW_BOOK:
		case ROW_PODCAST:
			if (r.u.album.cover[0]) {
				make_thumb(row, index, r.u.album.cover);
			}
			break;
		case ROW_ARTIST:
			make_letter(row, r.u.artist.name);
			break;
		case ROW_PLAYLIST:
			make_letter(row, r.u.playlist.title);
			break;
		case ROW_MORE:
			break;
		}
	}

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(texts, 0, 0);
	lv_obj_set_style_border_width(texts, 0, 0);
	lv_obj_set_style_pad_all(texts, 0, 0);
	lv_obj_set_style_pad_row(texts, SC(6), 0);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_event_bubble(texts, true);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *label = lv_label_create(texts);
	lv_label_set_text(label, name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_22));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);

	// What is playing, marked the way the player marks it: the accent colour.
	if (have && r.kind == ROW_TRACK && playing_id[0] && strcmp(playing_id, r.u.track.id) == 0) {
		lv_obj_set_style_text_color(label, theme()->accent, 0);
	}

	if (detail[0]) {
		lv_obj_t *sub = lv_label_create(texts);
		lv_label_set_text(sub, detail);
		lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
		lv_obj_set_width(sub, lv_pct(100));
		lv_obj_set_height(sub, lv_font_get_line_height(&font_ui_18));
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, &font_ui_18, 0);
	}

	if (have && row_menu_available(&r)) {
		lv_obj_t *btn = lv_btn_create(row);
		lv_obj_set_size(btn, SC(44), SC(44));
		lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(btn, 0, 0);
		lv_obj_set_style_shadow_width(btn, 0, 0);
		lv_obj_set_style_pad_all(btn, 0, 0);
		lv_obj_add_event_cb(btn, row_menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

		lv_obj_t *dots = lv_image_create(btn);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
		lv_obj_center(dots);
	}
}

// The rows on screen, as a range of indices, for listrow_visible(). Read from
// the row objects themselves: the container's flex layout is the only thing
// that knows where a row ended up.
static void list_visible_range(int *first, int *last) {
	*first = 0;
	*last = -1;
	int count = view_count();
	if (!list_container || count <= 0) {
		return;
	}
	int32_t top = lv_obj_get_scroll_y(list_container);
	int32_t height = lv_obj_get_height(list_container);
	int f = count, l = -1;
	for (int i = 0; i < count; i++) {
		lv_obj_t *row = lv_obj_get_child(list_container, i);
		if (!row) {
			continue;
		}
		int32_t y = lv_obj_get_y(row);
		int32_t h = lv_obj_get_height(row);
		if (y + h < top) {
			continue;
		}
		if (y > top + height) {
			break;
		}
		if (i < f) {
			f = i;
		}
		l = i;
	}
	if (l >= 0) {
		*first = f;
		*last = l;
	}
}

static void list_visible(int first, int last);

static void render_list(void) {
	if (!list_container) {
		return;
	}
	// A new list, or the same one redrawn: every row object goes, so every
	// cover this page asked for goes with it.
	listrow_reset((void *)&zvuk_art_owner);
	lv_obj_clean(list_container);

	int count = view_count();
	for (int i = 0; i < count; i++) {
		make_row(i);
	}

	if (shown && (list_kind == JOB_BOOK || list_kind == JOB_PODCAST) && shown->spoken.title[0]) {
		lv_label_set_text(list_title, shown->spoken.title);
	}

	if (shown && count == 0 && shown->error[0]) {
		lv_label_set_text(list_empty, shown->error);
		lv_obj_set_hidden(list_empty, false);
	} else if (shown && count == 0) {
		lv_label_set_text(list_empty, tr("no_results"));
		lv_obj_set_hidden(list_empty, false);
	} else {
		lv_obj_set_hidden(list_empty, true);
	}

	lv_obj_update_layout(list_container);
	int first, last;
	list_visible_range(&first, &last);
	list_visible(first, last);
}

// ---------------------------------------------------------------------------
// paging
// ---------------------------------------------------------------------------

static void request_page(int page) {
	if (!shown || !shown->list || page == page_asked || (page == page_failed && time(NULL) - page_failed_at < 5)) {
		return;
	}
	page_asked = page;
	start_worker();
	job_t job = {.kind = JOB_PAGE, .n = page, .generation = list_generation, .list = zvuklist_ref(shown->list)};
	submit(&job);
}

static void list_visible(int first, int last) {
	listrow_visible((void *)&zvuk_art_owner, first, last);
	int count = view_count();
	if (!shown || !shown->list || count <= 0) {
		return;
	}
	int pages[3] = {first / ZVUKLIST_PAGE, last / ZVUKLIST_PAGE, (last + ZVUKLIST_PAGE / 2) / ZVUKLIST_PAGE};
	for (int i = 0; i < 3; i++) {
		if (pages[i] * ZVUKLIST_PAGE < count && !zvuklist_page_ready(shown->list, pages[i])) {
			request_page(pages[i]);
			break;
		}
	}
}

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	int first, last;
	list_visible_range(&first, &last);
	list_visible(first, last);
}

// ---------------------------------------------------------------------------
// the list page's own history (see yandexpage.c in the fork)
//
// The screen manager sees one list screen, so opening an album from the results
// and going back would leave the page. The page keeps a stack of steps instead:
// the chevron consumes one before it lets the page close.
// ---------------------------------------------------------------------------

typedef struct {
	job_kind_t kind;
	char title[ZVUK_TITLE_MAX];
	char a[ZVUK_TITLE_MAX];
	char b[ZVUK_ID_MAX];
	int n;
} list_step_t;

#define LIST_HISTORY_DEPTH 8
static list_step_t list_history[LIST_HISTORY_DEPTH];
static int list_history_len;
static list_step_t list_current;
static bool list_has_current;

static void show_list(const list_step_t *step);

static bool same_list(const list_step_t *a, const list_step_t *b) {
	return a->kind == b->kind && !strcmp(a->a, b->a) && !strcmp(a->b, b->b) && a->n == b->n;
}

static bool list_back_has_step(void) { return list_history_len > 0; }

static bool list_back_guard(void) {
	if (list_history_len == 0) {
		list_has_current = false;
		return false;
	}
	list_current = list_history[--list_history_len];
	show_list(&list_current);
	return true;
}

static void note_list_step(const list_step_t *step) {
	if (lv_screen_active() != list_screen) {
		list_history_len = 0;
	} else if (list_has_current && !same_list(&list_current, step)) {
		if (list_history_len == LIST_HISTORY_DEPTH) {
			memmove(list_history, list_history + 1, (LIST_HISTORY_DEPTH - 1) * sizeof(list_history[0]));
			list_history_len--;
		}
		list_history[list_history_len++] = list_current;
	}
}

static void open_list(const char *title, job_kind_t kind, const char *a, const char *b, int n) {
	list_step_t step;
	memset(&step, 0, sizeof(step));
	step.kind = kind;
	step.n = n;
	snprintf(step.title, sizeof(step.title), "%s", title ? title : "");
	snprintf(step.a, sizeof(step.a), "%s", a ? a : "");
	snprintf(step.b, sizeof(step.b), "%s", b ? b : "");
	note_list_step(&step);
	list_current = step;
	list_has_current = true;
	show_list(&step);
}

// The saved tracks are an index on the card, not a request: the rows are built
// here, on the UI thread, and handed to the same handler a worker's answer
// takes.
static job_result_t *saved_result(job_kind_t kind, const char *album_id) {
	job_result_t *r = calloc(1, sizeof(*r) + MAX_ROWS * sizeof(row_t));
	zvuk_track_t *tracks = calloc(MAX_ROWS, sizeof(*tracks));
	if (!r || !tracks) {
		free(r);
		free(tracks);
		return NULL;
	}
	r->kind = kind;
	r->generation = list_generation;
	int count = 0;
	if (kind == JOB_SAVED) {
		zvuk_album_t *albums = (zvuk_album_t *)tracks;
		int n = zvukdl_albums(albums, MAX_ROWS);
		for (int i = 0; i < n; i++) {
			r->rows[count].kind = ROW_ALBUM;
			r->rows[count++].u.album = albums[i];
		}
		n = zvukdl_tracks(NULL, tracks, MAX_ROWS - count);
		for (int i = 0; i < n; i++) {
			r->rows[count].kind = ROW_TRACK;
			r->rows[count++].u.track = tracks[i];
		}
	} else {
		int n = zvukdl_tracks(album_id, tracks, MAX_ROWS);
		for (int i = 0; i < n; i++) {
			r->rows[count].kind = ROW_TRACK;
			r->rows[count++].u.track = tracks[i];
		}
	}
	free(tracks);
	r->count = count;
	if (count == 0) {
		snprintf(r->error, sizeof(r->error), "Nothing is saved on the card.");
	}
	return r;
}

static void show_list(const list_step_t *step) {
	list_kind = step->kind;
	lv_label_set_text(list_title, step->title);
	bool is_album = (step->kind == JOB_ALBUM || step->kind == JOB_SAVED_ALBUM || step->kind == JOB_BOOK ||
					 step->kind == JOB_PODCAST) &&
					step->a[0];
	if (is_album) {
	}
	lv_obj_set_hidden(list_empty, true);
	lv_obj_set_hidden(list_spin, true);
	page_asked = -1;
	if (lv_screen_active() != list_screen) {
		switch_screen(list_screen);
	}
	list_generation++;
	free_result(shown);
	shown = NULL;
	render_list();
	if (step->kind == JOB_SAVED || step->kind == JOB_SAVED_ALBUM) {
		job_result_t *r = saved_result(step->kind, step->a);
		if (r) {
			job_done_async(r);
		}
		return;
	}
	lv_obj_set_hidden(list_spin, false);
	run(step->kind, step->a, step->b, step->n);
}

// ---------------------------------------------------------------------------
// playback
// ---------------------------------------------------------------------------

static void playback_event(zvukplay_event_t event, const char *text) {
	if (list_spin) {
		lv_obj_set_hidden(list_spin, true);
	}
	switch (event) {
	case ZVUKPLAY_STARTED:
		player_refresh_now_playing();
		if (playing_update()) {
			render_list();
		}
		break;
	case ZVUKPLAY_FAILED:
		toast_error(text[0] ? text : tr("download_failed"));
		break;
	case ZVUKPLAY_SKIPPED: {
		char line[320];
		snprintf(line, sizeof(line), "Skipped: %s", text);
		toast_error(line);
		break;
	}
	}
}

static void play_rows(int tapped) {
	row_t r;
	bool have = view_row(tapped, &r);
	if (have && r.kind == ROW_TRACK && !r.u.track.available) {
		toast_error("This track cannot be played.");
		return;
	}
	// A saved episode plays its own file, from where it was left.
	if (have && r.kind == ROW_TRACK && r.u.track.content == ZVUK_CONTENT_EPISODE) {
		char path[512];
		if (zvukdl_local_episode(&r.u.track, path, sizeof(path))) {
			const char *one[] = {path};
			device_state_play_list_ordered_at(one, 1, 0, podcaststate_resume_point(path));
			player_refresh_now_playing();
			return;
		}
	}
	lv_obj_set_hidden(list_spin, false);
	if (shown && shown->list) {
		if (!zvukplay_list(shown->list, tapped)) {
			lv_obj_set_hidden(list_spin, true);
		}
		return;
	}
	// A list of mixed rows (an artist's page, a search): the queue is the
	// playable tracks among them, in the order they are shown.
	zvuklist_t *tracks = zvuklist_new();
	int start = -1;
	int count = view_count();
	for (int i = 0; tracks && i < count; i++) {
		row_t row;
		if (view_row(i, &row) && row.kind == ROW_TRACK && row.u.track.available &&
			zvuklist_add_track(tracks, &row.u.track) && i == tapped) {
			start = zvuklist_count(tracks) - 1;
		}
	}
	if (start < 0 || !zvukplay_list(tracks, start)) {
		lv_obj_set_hidden(list_spin, true);
	}
	zvuklist_free(tracks);
}

static void row_clicked_cb(lv_event_t *e) {
	// A gesture that ends over a row is not a tap on it: events bubble.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= view_count()) {
		return;
	}
	row_t r;
	if (!view_row(index, &r)) {
		if (shown && shown->list) {
			play_rows(index);
		}
		return;
	}
	switch (r.kind) {
	case ROW_TRACK:
		play_rows(index);
		break;
	case ROW_MORE:
		if (list_current.kind == JOB_BROWSE_BOOKS) {
			open_list(list_current.title, JOB_BROWSE_BOOKS, NULL, list_current.b, list_current.n + BROWSE_BOOKS_PAGE);
		}
		break;
	case ROW_ALBUM:
		open_list(r.u.album.title, list_kind == JOB_SAVED ? JOB_SAVED_ALBUM : JOB_ALBUM, r.u.album.id, NULL, 0);
		break;
	case ROW_BOOK:
	case ROW_PODCAST:
		open_list(r.u.album.title, r.kind == ROW_BOOK ? JOB_BOOK : JOB_PODCAST, r.u.album.id, NULL, 0);
		break;
	case ROW_ARTIST:
		open_list(r.u.artist.name, JOB_ARTIST, r.u.artist.id, NULL, 0);
		break;
	case ROW_PLAYLIST:
		open_list(r.u.playlist.title, JOB_PLAYLIST, r.u.playlist.id, NULL, 0);
		break;
	}
}

// ---------------------------------------------------------------------------
// the row menu: a like, a download, the card
//
// The fork's long-press sheet (sx_sheet_menu). Here it is the popover this
// player puts on a row's ellipsis, with the actions the port carries: the like
// and what is on the card. The queue actions are not among them -- see the
// comment at the top.
// ---------------------------------------------------------------------------

static row_t menu_row;

static void refresh_after_change(void) {
	if (lv_screen_active() == list_screen) {
		render_list();
	}
}

static void like_done_async(void *user) {
	char *why = user;
	if (why) {
		toast_error(why[0] ? why : "The service refused the change.");
		free(why);
	}
	refresh_after_change();
}

static void like_done(bool ok, const char *error, void *user) {
	(void)user;
	char *why = NULL;
	if (!ok) {
		why = strdup(error && error[0] ? error : "");
		if (!why) {
			return;
		}
	}
	if (!gui_post(like_done_async, why)) {
		free(why);
	}
}

static void do_like(void *user) {
	bool want = user != NULL;
	switch (menu_row.kind) {
	case ROW_TRACK:
		zvuksync_like(ZVUK_ITEM_TRACK, menu_row.u.track.id, want, like_done, NULL);
		break;
	case ROW_ALBUM:
		zvuksync_like(ZVUK_ITEM_RELEASE, menu_row.u.album.id, want, like_done, NULL);
		break;
	case ROW_ARTIST:
		zvuksync_like(ZVUK_ITEM_ARTIST, menu_row.u.artist.id, want, like_done, NULL);
		break;
	case ROW_PLAYLIST:
		zvuksync_like(ZVUK_ITEM_PLAYLIST, menu_row.u.playlist.id, want, like_done, NULL);
		break;
	default:
		return;
	}
	refresh_after_change();
}

static void save_refused(void) { toast_error("Nothing was saved: the card or the session refused."); }

static void do_save(void *user) {
	(void)user;
	bool ok = false;
	switch (menu_row.kind) {
	case ROW_TRACK:
		ok = menu_row.u.track.content == ZVUK_CONTENT_CHAPTER ? zvukdl_book(menu_row.u.track.album_id, menu_row.u.track.album)
															 : zvukdl_track(&menu_row.u.track);
		break;
	case ROW_ALBUM:
		ok = zvukdl_album(menu_row.u.album.id, menu_row.u.album.title);
		break;
	case ROW_BOOK:
		ok = zvukdl_book(menu_row.u.album.id, menu_row.u.album.title);
		break;
	default:
		break;
	}
	if (!ok) {
		save_refused();
	}
}

static void after_delete(void) {
	refresh_after_change();
	rebuild_home();
}

static void delete_confirmed(void *user) {
	(void)user;
	bool ok;
	if (menu_row.kind == ROW_ALBUM) {
		ok = zvukdl_delete_album(menu_row.u.album.id) > 0 || zvukdl_album_saved_count(menu_row.u.album.id) == 0;
	} else {
		ok = zvukdl_delete(menu_row.u.track.id);
	}
	if (ok) {
		toast_success("Deleted from the card.");
	} else {
		toast_error("Nothing was deleted.");
	}
	after_delete();
}

static void do_delete(void *user) {
	(void)user;
	const char *name = menu_row.kind == ROW_ALBUM ? menu_row.u.album.title : menu_row.u.track.title;
	confirm_show("Delete from the card?", name, "delete", delete_confirmed, NULL);
}

static void row_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e); // the tap belongs to the menu, not to the row
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || !view_row(index, &menu_row)) {
		return;
	}

	popover_item_t items[3];
	memset(items, 0, sizeof(items));
	int n = 0;

	switch (menu_row.kind) {
	case ROW_TRACK:
		if (menu_row.u.track.content == ZVUK_CONTENT_TRACK) {
			bool liked = zvuk_is_liked(ZVUK_ITEM_TRACK, menu_row.u.track.id);
			items[n].label = liked ? "remove_from_favourites" : "medialist_add_to_favourites";
			items[n].action = do_like;
			items[n].user = liked ? NULL : (void *)1;
			n++;
		}
		if (row_saved(&menu_row)) {
			items[n].label = "Delete from the card";
			items[n].action = do_delete;
			n++;
		} else if (menu_row.u.track.available) {
			items[n].label = menu_row.u.track.content == ZVUK_CONTENT_CHAPTER ? "Download the book" : "Save to the card";
			items[n].action = do_save;
			n++;
		}
		break;
	case ROW_ALBUM: {
		bool liked = zvuk_is_liked(ZVUK_ITEM_RELEASE, menu_row.u.album.id);
		items[n].label = liked ? "remove_from_favourites" : "medialist_add_to_favourites";
		items[n].action = do_like;
		items[n].user = liked ? NULL : (void *)1;
		n++;
		if (zvukdl_album_saved_count(menu_row.u.album.id) > 0) {
			items[n].label = "Delete from the card";
			items[n].action = do_delete;
			n++;
		} else {
			items[n].label = "Save to the card";
			items[n].action = do_save;
			n++;
		}
		break;
	}
	case ROW_ARTIST: {
		bool liked = zvuk_is_liked(ZVUK_ITEM_ARTIST, menu_row.u.artist.id);
		items[n].label = liked ? "remove_from_favourites" : "medialist_add_to_favourites";
		items[n].action = do_like;
		items[n].user = liked ? NULL : (void *)1;
		n++;
		break;
	}
	case ROW_PLAYLIST: {
		bool liked = zvuk_is_liked(ZVUK_ITEM_PLAYLIST, menu_row.u.playlist.id);
		items[n].label = liked ? "remove_from_favourites" : "medialist_add_to_favourites";
		items[n].action = do_like;
		items[n].user = liked ? NULL : (void *)1;
		n++;
		break;
	}
	case ROW_BOOK:
		// A book is one download or nothing: there is no delete for the
		// audiobook shelf in this player's index, so a saved book says so and
		// offers nothing.
		if (row_saved(&menu_row)) {
			items[n].label = "Saved on the card";
			items[n].checked = true;
		} else {
			items[n].label = "Download the book";
			items[n].action = do_save;
		}
		n++;
		break;
	default:
		return;
	}

	if (n == 0) {
		return;
	}
	popover_show(lv_event_get_current_target(e), items, n);
}

// ---------------------------------------------------------------------------
// the list screen
// ---------------------------------------------------------------------------

static void list_loaded_cb(lv_event_t *e) {
	(void)e;
	if (!shown) {
		return;
	}
	// Coming back to the list: the rows are where they were, so nothing is
	// rebuilt -- only the visible range is told again (the covers of a row that
	// scrolled away were given back) and the playing row is repainted if the
	// track changed while the page was away.
	if (playing_update()) {
		int32_t keep = lv_obj_get_scroll_y(list_container);
		render_list();
		lv_obj_scroll_to_y(list_container, keep, LV_ANIM_OFF);
		return;
	}
	lv_obj_update_layout(list_container);
	int first, last;
	list_visible_range(&first, &last);
	list_visible(first, last);
}

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	// The shell knows the screen init() returns and nothing else unless it is
	// told: this one it has to take off the display and delete at unload.
	sonix_addon_own_screen(addon_id, list_screen);
	list_container = settingsrow_page(list_screen, cfg, "Zvuk");
	list_title = settingsrow_page_title(list_screen);

	list_empty = lv_label_create(list_screen);
	lv_label_set_text(list_empty, tr("no_results"));
	lv_label_set_long_mode(list_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(list_empty, cfg->screen_width - 2 * cfg->padding - SC(12));
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_22, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + SC(100));
	lv_obj_set_hidden(list_empty, true);

	list_spin = spinner_create(list_screen, &icon_loader_big);
	lv_obj_align(list_spin, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + SC(40));
	lv_obj_set_hidden(list_spin, true);

	lv_obj_add_event_cb(list_container, list_scrolled_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_add_event_cb(list_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(list_screen);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	player_sheet_attach_drag(list_screen, true);
	player_sheet_attach_drag(list_container, true);
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

static lv_obj_t *search_field;
static keyboard_t *search_keyboard;
static lv_obj_t *search_pills[ZVUK_SEARCH_KINDS];
static int search_kind_index;

static void paint_search_pills(void) {
	for (int i = 0; i < ZVUK_SEARCH_KINDS; i++) {
		settingsrow_pill_active(search_pills[i], i == search_kind_index);
	}
}

static void search_pill_cb(lv_event_t *e) {
	search_kind_index = (int)(intptr_t)lv_event_get_user_data(e);
	paint_search_pills();
}

// A link to a book or a show opens it, as the fork's page did: the address is
// the id.
static bool spoken_link(const char *text, job_kind_t *kind, char *id, size_t size) {
	static const struct {
		const char *prefix;
		job_kind_t kind;
	} links[] = {{"https://zvuk.com/abook/", JOB_BOOK}, {"https://zvuk.com/podcast/", JOB_PODCAST}};
	for (size_t i = 0; text && i < sizeof(links) / sizeof(links[0]); i++) {
		size_t prefix = strlen(links[i].prefix);
		if (strncmp(text, links[i].prefix, prefix)) {
			continue;
		}
		const char *number = text + prefix;
		size_t len = strspn(number, "0123456789");
		if (!len || len >= size || len >= ZVUK_ID_MAX ||
			(number[len] && number[len] != '?' && number[len] != '#')) {
			return false;
		}
		snprintf(id, size, "%.*s", (int)len, number);
		*kind = links[i].kind;
		return true;
	}
	return false;
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !text[0]) {
		return;
	}
	job_kind_t spoken;
	char id[ZVUK_ID_MAX];
	if (spoken_link(text, &spoken, id, sizeof(id))) {
		open_list(spoken == JOB_BOOK ? "Audiobook" : "Podcast", spoken, id, NULL, 0);
		return;
	}
	open_list(text, JOB_SEARCH, text, NULL, search_kind_index);
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (!zvuk_logged_in()) {
		gui_notify_popup("Sign in first: search needs the account.");
		return;
	}
	switch_screen(search_screen);
}

static void build_search_page(gui_config_t *cfg) {
	search_screen = lv_obj_create(NULL);
	// Registered for the same reason as the list screen (see build_list_page).
	sonix_addon_own_screen(addon_id, search_screen);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, cfg, "Search");
	int top = settingsrow_content_top(cfg);

	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("artist_album_or_track"));
	lv_obj_set_size(search_field, cfg->screen_width - 2 * cfg->padding, SC(62));
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, SC(12), 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, SC(14), 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);

	// The kinds of result, as pills: a filter that replaces the list rather
	// than a step to go back to.
	lv_obj_t *pills = lv_obj_create(search_screen);
	lv_obj_set_size(pills, cfg->screen_width - 2 * cfg->padding, LV_SIZE_CONTENT);
	lv_obj_align(pills, LV_ALIGN_TOP_LEFT, cfg->padding, top + SC(76));
	lv_obj_set_style_bg_opa(pills, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, SC(10), 0);
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	static const char *const KINDS[ZVUK_SEARCH_KINDS] = {"Tracks",	 "Albums",	  "Artists",
														 "Playlists", "Audiobooks", "Podcasts"};
	for (int i = 0; i < ZVUK_SEARCH_KINDS; i++) {
		search_pills[i] = settingsrow_pill_text(pills, KINDS[i], i, search_pill_cb);
	}
	paint_search_pills();

	search_keyboard = keyboard_create(search_screen, cfg->screen_width, SC(316), search_field, NULL, "ok",
									  search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
	theme_register_refresh(paint_search_pills);
}

// ---------------------------------------------------------------------------
// the token on the card, and the account
// ---------------------------------------------------------------------------

static void token_path(char *out, size_t size) {
	if (!addon_dir[0]) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%s/%s", addon_dir, ZVUK_TOKEN_FILE);
}

// The first line of the token file that has anything on it. A whole
// profile answer pasted in is one line too, and zvuk_token_clean() takes the
// token out of it.
static bool read_token_file(char *out, size_t size) {
	char path[520];
	out[0] = '\0';
	token_path(path, sizeof(path));
	FILE *f = path[0] ? fopen(path, "r") : NULL;
	if (!f) {
		return false;
	}
	char line[ZVUK_TOKEN_MAX * 2];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *start = line;
		// A UTF-8 BOM, as an editor on Windows leaves it.
		if ((unsigned char)start[0] == 0xef && (unsigned char)start[1] == 0xbb && (unsigned char)start[2] == 0xbf) {
			start += 3;
		}
		while (*start == ' ' || *start == '\t') {
			start++;
		}
		if (*start) {
			snprintf(out, size, "%.*s", (int)(size - 1), start);
			break;
		}
	}
	fclose(f);
	return out[0] != '\0';
}

static bool token_file_present(void) {
	char path[520];
	token_path(path, sizeof(path));
	if (!path[0]) {
		return false;
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	fclose(f);
	return true;
}

// ---------------------------------------------------------------------------
// the home page
// ---------------------------------------------------------------------------

static lv_obj_t *home_container;
static lv_obj_t *quality_pills[ZVUK_QUALITY_COUNT];
static lv_obj_t *quality_note;
static bool profile_asked;
static bool sign_in_running;

static void start_sign_in(void);
static void check_connection_cb(lv_event_t *e);

static void quality_refresh(void) {
	zvuk_quality_t current = zvuk_get_quality();
	for (int i = 0; i < ZVUK_QUALITY_COUNT; i++) {
		if (quality_pills[i]) {
			settingsrow_pill_active(quality_pills[i], (int)current == i);
		}
	}
	if (!quality_note) {
		return;
	}
	char format[48];
	zvuk_last_format(format, sizeof(format));
	if (current != ZVUK_QUALITY_MID && zvuk_needs_subscription()) {
		lv_label_set_text(quality_note, "The subscription does not cover this: MP3 128 comes instead.");
	} else if (format[0]) {
		lv_label_set_text_fmt(quality_note, "Last file: %s", format);
	} else {
		lv_label_set_text(quality_note, "A ceiling: a track without FLAC comes as MP3.");
	}
}

static void quality_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	zvuk_set_quality((zvuk_quality_t)(intptr_t)lv_event_get_user_data(e));
	quality_refresh();
}

static void simple_job_cb(lv_event_t *e) {
	job_kind_t kind = (job_kind_t)(intptr_t)lv_event_get_user_data(e);
	static const char *const TITLES[] = {
		[JOB_LIKED] = "Liked tracks",
		[JOB_ALBUMS] = "Albums",
		[JOB_ARTISTS] = "Artists",
		[JOB_PLAYLISTS] = "Playlists",
		[JOB_BROWSE_PICKS] = "Picks",
		[JOB_SAVED] = "Saved on the card",
	};
	const char *title = (unsigned)kind < sizeof(TITLES) / sizeof(TITLES[0]) && TITLES[kind] ? TITLES[kind] : "Zvuk";
	open_list(title, kind, NULL, NULL, 0);
}

static void books_cb(lv_event_t *e) {
	bool children = (intptr_t)lv_event_get_user_data(e) != 0;
	open_list(children ? "Audiobooks for children" : "Audiobooks", JOB_BROWSE_BOOKS, NULL, children ? "children" : "",
			  0);
}

static void open_saved(void) { open_list("Saved on the card", JOB_SAVED, NULL, NULL, 0); }

static void sign_out_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	zvukdl_cancel_all();
	zvuk_logout();
	zvukplay_signed_out();
	profile_asked = false;
	rebuild_home();
	toast_success(tr("signed_out"));
}

static void check_again_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (sign_in_running) {
		gui_notify_popup("The token is being read.");
		return;
	}
	start_sign_in();
}


static lv_obj_t *note_label(lv_obj_t *parent, const char *text) {
	lv_obj_t *note = lv_label_create(parent);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_20, 0);
	lv_label_set_text(note, text);
	return note;
}

static void open_saved_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	open_saved();
}

static void rebuild_home(void) {
	if (!home_container) {
		return;
	}
	lv_obj_clean(home_container);
	quality_note = NULL;
	for (int i = 0; i < ZVUK_QUALITY_COUNT; i++) {
		quality_pills[i] = NULL;
	}

	bool in = zvuk_logged_in();

	// Who is signed in, and what it covers.
	char name[ZVUK_NAME_MAX], plan[64];
	zvuk_account(name, sizeof(name), plan, sizeof(plan), NULL);
	lv_obj_t *value = NULL;
	settingsrow_add(home_container, "Account", &value, NULL, NULL);
	lv_label_set_text(value, in ? (name[0] ? name : tr("signed_in")) : "Not signed in");
	lv_obj_set_style_text_font(value, &font_ui_20, 0);

	if (in) {
		settingsrow_add(home_container, "Subscription", &value, NULL, NULL);
		lv_label_set_text(value, plan[0] ? plan : "-");
		lv_obj_set_style_text_font(value, &font_ui_20, 0);

		settingsrow_add(home_container, "Liked tracks", NULL, simple_job_cb, (void *)(intptr_t)JOB_LIKED);
		settingsrow_add(home_container, "Albums", NULL, simple_job_cb, (void *)(intptr_t)JOB_ALBUMS);
		settingsrow_add(home_container, "Artists", NULL, simple_job_cb, (void *)(intptr_t)JOB_ARTISTS);
		settingsrow_add(home_container, "Playlists", NULL, simple_job_cb, (void *)(intptr_t)JOB_PLAYLISTS);
		settingsrow_add(home_container, "Picks", NULL, simple_job_cb, (void *)(intptr_t)JOB_BROWSE_PICKS);
		settingsrow_add(home_container, "Audiobooks", NULL, books_cb, (void *)(intptr_t)0);
		settingsrow_add(home_container, "Audiobooks for children", NULL, books_cb, (void *)(intptr_t)1);

		char saved[64];
		snprintf(saved, sizeof(saved), "%d", zvukdl_saved_count());
		settingsrow_add(home_container, "Saved on the card", &value, open_saved_cb, NULL);
		lv_label_set_text(value, saved);
		lv_obj_set_style_text_font(value, &font_ui_20, 0);
	} else {
		// Without a session there is nothing to ask the service: the page says
		// where the token goes and offers to read the card again.
		note_label(home_container,
				   "Put the token from zvuk.com/api/tiny/profile into Addons/zvuk/token.txt on the card, "
				   "then tap \"Sign in from the card\".");
		note_label(home_container, token_file_present() ? "The file is there: sign in from the card."
													   : "No Addons/zvuk/token.txt on the card.");
		settingsrow_action(home_container, "Sign in from the card", check_again_cb, NULL);
		settingsrow_add(home_container, "Saved on the card", NULL, open_saved_cb, NULL);
	}

	// The quality ceiling: a card of pills, as every other choice here.
	lv_obj_t *pills = NULL;
	settingsrow_pills(home_container, "audio_quality", &pills);
	static const char *const QUALITY_NAMES[ZVUK_QUALITY_COUNT] = {"FLAC", "MP3 320", "MP3 128"};
	for (int i = 0; i < ZVUK_QUALITY_COUNT; i++) {
		quality_pills[i] = settingsrow_pill_text(pills, QUALITY_NAMES[i], i, quality_pick_cb);
	}
	quality_note = note_label(home_container, "");

	if (in) {
		settingsrow_action(home_container, "Check connection", check_connection_cb, NULL);
		settingsrow_action(home_container, "Sign in again from the card", check_again_cb, NULL);
		settingsrow_action(home_container, "Sign out", sign_out_cb, NULL);
	}

	quality_refresh();
}

// ---------------------------------------------------------------------------
// the workers the page starts itself: the token, the check
// ---------------------------------------------------------------------------

typedef struct {
	bool ok;
	char message[512];
} answer_t;

static void answer_async(void *user) {
	answer_t *a = user;
	if (a->message[0]) {
		gui_notify_popup(a->message);
	}
	sign_in_running = false;
	rebuild_home();
	free(a);
}

static void *sign_in_main(void *arg) {
	(void)arg;
	// Counted by start_sign_in() before this thread existed: see worker_main().
	answer_t *a = calloc(1, sizeof(*a));
	if (!a) {
		zvuk_stop_leave(&threads);
		return NULL;
	}
	char token[ZVUK_TOKEN_MAX];
	if (!read_token_file(token, sizeof(token))) {
		snprintf(a->message, sizeof(a->message), "No Addons/%s/token.txt on the card.", addon_id[0] ? addon_id : "zvuk");
		gui_post(answer_async, a);
		zvuk_stop_leave(&threads);
		return NULL;
	}
	cardimport_result_t result = zvuk_sign_in(token, a->message, sizeof(a->message));
	a->ok = result == CARDIMPORT_OK;
	if (!a->message[0]) {
		snprintf(a->message, sizeof(a->message), "%s", a->ok ? tr("signed_in") : "The token was not accepted.");
	}
	memset(token, 0, sizeof(token));
	if (!gui_post(answer_async, a)) {
		free(a);
	}
	zvuk_stop_leave(&threads);
	return NULL;
}

static void start_sign_in(void) {
	if (stopping) {
		return;
	}
	sign_in_running = true;
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	zvuk_stop_enter(&threads);
	if (pthread_create(&th, &attr, sign_in_main, NULL) != 0) {
		zvuk_stop_leave(&threads);
		sign_in_running = false;
		gui_notify_popup("Could not start a worker.");
	}
	pthread_attr_destroy(&attr);
}

static void show_check_report(const char *report) {
	confirm_notice("Connection check", report && report[0] ? report : "Passed.");
}

static void check_connection_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (!zvuk_logged_in()) {
		confirm_notice("Connection check", "Not signed in: put a token on the card first.");
		return;
	}
	toast_busy("Checking the connection...");
	run(JOB_CHECK, NULL, NULL, 0);
}

// The downloads talk to the interface through the worker: the listener runs
// there, and gui_post() brings each event over.
typedef struct {
	zvukdl_event_t event;
	int count;
	char text[400];
} save_note_t;

static void save_note_async(void *user) {
	save_note_t *note = user;
	char line[480];
	switch (note->event) {
	case ZVUKDL_QUEUED:
		snprintf(line, sizeof(line), "Saving: %s", note->text);
		toast_success(line);
		break;
	case ZVUKDL_SAVED:
		refresh_after_change();
		rebuild_home();
		break;
	case ZVUKDL_BOOK_SAVED:
	case ZVUKDL_EPISODE_SAVED:
		refresh_after_change();
		rebuild_home();
		toast_success(note->event == ZVUKDL_BOOK_SAVED ? "The book is on the card." : "The episode is on the card.");
		break;
	case ZVUKDL_FAILED:
		toast_error(note->text);
		break;
	case ZVUKDL_FINISHED:
		if (note->count > 0) {
			snprintf(line, sizeof(line), "%d saved to %s", note->count, note->text);
			toast_success(line);
		}
		refresh_after_change();
		rebuild_home();
		break;
	case ZVUKDL_STOPPED:
		toast_plain(note->text);
		break;
	}
	free(note);
}

static void save_listener(zvukdl_event_t event, int count, const char *text) {
	save_note_t *note = malloc(sizeof(*note));
	if (!note) {
		return;
	}
	note->event = event;
	note->count = count;
	snprintf(note->text, sizeof(note->text), "%s", text ? text : "");
	if (!gui_post(save_note_async, note)) {
		free(note);
	}
}

// ---------------------------------------------------------------------------
// what comes back from the worker, on the GUI thread
// ---------------------------------------------------------------------------

static void job_done_async(void *user) {
	job_result_t *r = user;
	switch (r->kind) {
	case JOB_PROFILE:
		rebuild_home();
		free_result(r);
		return;
	case JOB_CHECK:
		toast_busy_end();
		show_check_report(r->error);
		rebuild_home();
		free_result(r);
		return;
	case JOB_PAGE: {
		if (!shown || r->list != shown->list || r->generation != list_generation) {
			free_result(r);
			return;
		}
		if (page_asked == r->page) {
			page_asked = -1;
		}
		if (r->error[0]) {
			page_failed = r->page;
			page_failed_at = time(NULL);
		}
		if (!r->error[0]) {
			playing_update();
			// Another page of details arrived. The list is redrawn from
			// `shown`, so the reader's place is taken before the rows are
			// rebuilt and put back after: arriving details must not throw the
			// list back to its first row.
			int32_t keep = lv_obj_get_scroll_y(list_container);
			render_list();
			lv_obj_scroll_to_y(list_container, keep, LV_ANIM_OFF);
		}
		free_result(r);
		return;
	}
	default:
		break;
	}
	if (r->generation != list_generation) {
		free_result(r);
		return;
	}
	lv_obj_set_hidden(list_spin, true);
	free_result(shown);
	shown = r;
	playing_update();
	render_list();
	// A session that just ended (the token expired) shows on the home too.
	if (!zvuk_logged_in()) {
		rebuild_home();
	}
	zvuksync_likes_refresh(NULL);
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	// Never on the way out: this is where a token on the card starts a sign-in
	// and where the profile is asked for, and stop() has just waited for
	// threads like those.
	if (stopping) {
		return;
	}
	// The card may have come or gone since the page was built.
	listart_set_root(card_root[0] ? card_root : NULL);
	rebuild_home();
	// A token put on the card while the page was away is picked up here, with
	// no restart. Without one there is no request at all.
	if (!zvuk_logged_in() && !sign_in_running && token_file_present()) {
		start_sign_in();
		return;
	}
	if (zvuk_logged_in() && !profile_asked) {
		profile_asked = true; // the name and the plan, once a session
		run(JOB_PROFILE, NULL, NULL, 0);
	}
	if (zvuk_logged_in()) {
		zvuksync_likes_refresh(NULL);
	}
}

static void *open_album_for_player(const char *album_id, const char *title) {
	if (!album_id || !album_id[0] || !zvuk_logged_in()) {
		return NULL;
	}
	open_list(title && title[0] ? title : "Zvuk", JOB_ALBUM, album_id, NULL, 0);
	return list_screen;
}

void *zvukpage_open_album(const char *album_id, const char *title) { return open_album_for_player(album_id, title); }

void zvukpage_init(gui_config_t *cfg, const char *id, const char *sd_root, const char *dir) {
	page_cfg = cfg;
	snprintf(addon_id, sizeof(addon_id), "%s", id ? id : "");
	snprintf(card_root, sizeof(card_root), "%s", sd_root ? sd_root : "");
	snprintf(addon_dir, sizeof(addon_dir), "%s", dir ? dir : "");
	listart_set_root(card_root[0] ? card_root : NULL);

	zvukpage_screen = lv_obj_create(NULL);
	// init()'s own screen is registered here too: the shell records the one
	// init() returns by itself, and this makes the list of screens this library
	// owns independent of that one field (the call ignores a repeat).
	sonix_addon_own_screen(addon_id, zvukpage_screen);
	home_container = settingsrow_page(zvukpage_screen, cfg, "Zvuk");
	// One corner button, the magnifier: search is not a row among the others.
	settingsrow_title_corner_slots(settingsrow_page_title(zvukpage_screen), cfg, 1);
	lv_obj_t *search_btn = lv_btn_create(zvukpage_screen);
	lv_obj_set_size(search_btn, SC(56), SC(56));
	lv_obj_set_style_bg_opa(search_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(search_btn, 0, 0);
	lv_obj_set_style_shadow_width(search_btn, 0, 0);
	lv_obj_set_style_pad_all(search_btn, 0, 0);
	lv_obj_align(search_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(search_btn, open_search_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *search_icon = lv_image_create(search_btn);
	lv_image_set_src(search_icon, &icon_search);
	lv_obj_add_style(search_icon, &theme_style_icon, 0);
	lv_obj_center(search_icon);

	build_list_page(cfg);
	build_search_page(cfg);

	rebuild_home();

	lv_obj_add_event_cb(zvukpage_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(zvukpage_screen);
	player_sheet_attach_drag(zvukpage_screen, true);
	player_sheet_attach_drag(home_container, true);
	// The second of the two theme hooks; the first came with the search page.
	// One flag for both: zvukpage_stop() is the only place that takes them back.
	theme_register_refresh(quality_refresh);
	theme_registered = true;

	// The downloads report here, so the list and the home stay true while
	// something is being saved.
	zvukdl_set_listener(save_listener);
	// Playback's events: the spinner, the playing row, a failure in words.
	zvukplay_init(playback_event);

	// The mark over the artwork and "show album" belong to the service table,
	// which the page is the only part of the player able to fill. There is no
	// Zvuk badge in this player's icon set, so the artwork keeps none.
	zvuk_service_attach_gui(NULL, open_album_for_player);
}

// ---------------------------------------------------------------------------
// stopping
// ---------------------------------------------------------------------------

bool zvukpage_stop(void) {
	// Nothing new starts from here: the stops below wait for threads, and a
	// handler that ran in between would start one of them again.
	pthread_mutex_lock(&job_lock);
	stopping = true;
	zvuklist_free(page_pending.list);
	page_pending = (job_t){0};
	pending = (job_t){0};
	events_count = 0;
	pthread_cond_broadcast(&job_cond);
	pthread_mutex_unlock(&job_lock);

	// The two theme hooks are code here: a theme or accent change after the
	// unload would call into the mapping.
	if (theme_registered) {
		theme_unregister_refresh(paint_search_pills);
		theme_unregister_refresh(quality_refresh);
		theme_registered = false;
	}

	// No more reports from the save queue, and the covers of the rows on screen
	// go back to listart before the rows themselves are deleted -- by the shell,
	// after this returns, one screen at a time. listrow_stop() takes the poll
	// timer with it: it is not a child of any widget and its callback is code in
	// this library.
	zvukdl_set_listener(NULL);
	listrow_reset((void *)&zvuk_art_owner);
	listrow_stop();

	// A row menu left open is on the player's top layer, not on this screen: it
	// would keep the tap that runs do_like()/do_save() -- code here -- reachable
	// over whatever page came next. The same for the confirm card the delete
	// path opens, and for a "checking the connection" toast, which is otherwise
	// never taken down by itself.
	popover_close();
	confirm_close();
	if (toast_busy_showing()) {
		toast_busy_end();
	}

	// The worker, and any sign-in it started. A job in flight is a request with
	// a cap of its own (STREAM_UI_TIMEOUT_SECS), so the wait is the contract and
	// not a promise; false is the thread it could not outlast.
	bool gone = zvuk_stop_wait(&threads, "page worker");

	// The result array is the worker's own scratch space, so it goes only when
	// the thread that writes it is gone; `page_pending` above is what a job of
	// the list's next page held.
	pthread_mutex_lock(&job_lock);
	if (gone) {
		worker_started = false;
		stopping = false; // a second init() starts a worker again
		free(result);
		result = NULL;
		zvuklist_free(result_list);
		result_list = NULL;
	}
	list_generation++;
	pthread_mutex_unlock(&job_lock);

	// The list on screen: its ids are this page's, the player's playlist is not
	// the same thing and does not point here.
	free_result(shown);
	shown = NULL;

	// Everything below pointed into the screens the shell deletes once this
	// returns, and the next run would write through it.
	list_container = NULL;
	list_title = NULL;
	list_empty = NULL;
	list_spin = NULL;
	search_field = NULL;
	search_keyboard = NULL;
	quality_note = NULL;
	home_container = NULL;
	zvukpage_screen = NULL;
	list_screen = NULL;
	search_screen = NULL;
	for (int i = 0; i < ZVUK_SEARCH_KINDS; i++) {
		search_pills[i] = NULL;
	}
	for (int i = 0; i < ZVUK_QUALITY_COUNT; i++) {
		quality_pills[i] = NULL;
	}

	// And the state, so that a second init() in the same process starts where
	// the first one did rather than on what the last one happened to show.
	page_cfg = NULL;
	addon_id[0] = '\0';
	card_root[0] = '\0';
	addon_dir[0] = '\0';
	list_kind = JOB_NONE;
	page_asked = -1;
	page_failed = -1;
	page_failed_at = 0;
	playing_id[0] = '\0';
	list_history_len = 0;
	list_has_current = false;
	memset(&list_current, 0, sizeof(list_current));
	memset(&menu_row, 0, sizeof(menu_row));
	search_kind_index = 0;
	profile_asked = false;
	sign_in_running = false;
	result_count = 0;
	result_error[0] = '\0';
	result_class = STREAM_ERR_NONE;
	memset(&result_spoken, 0, sizeof(result_spoken));

	// The service table is in this library too, and its two GUI halves point at
	// this page. The player unregisters the table itself; emptying it here is
	// for the unload that fails and leaves the library mapped.
	zvuk_service_attach_gui(NULL, NULL);
	return gone;
}
