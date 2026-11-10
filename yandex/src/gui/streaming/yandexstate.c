#include "yandexstate.h"

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
#include "src/system/playback/playlist.h"
#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexconf.h"
#include "src/system/streaming/yandexdl.h"
#include "src/system/streaming/yandexlist.h"
#include "src/system/streaming/yandexlive.h"
#include "src/system/streaming/yandexplay.h"
#include "src/system/streaming/yandexsaved.h"
#include "src/system/streaming/yandexservice.h"
#include "src/system/streaming/yandexsync.h"
#include "src/system/streaming/yandexwave.h"

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// Yandex Music, the page.
//
// Three screens, the shape Zvuk's page has (addons/zvuk/src/gui/streaming/
// zvukpage.c is the skeleton this was built from), because the two services are
// brothers: a home of sections, one list screen that every section and every
// detail page reuses, and a search page. What the page itself owns is only what
// is drawing and what is chosen; everything that talks to the network was here
// already -- yandex.c (the API and the pacing), yandexlist.c (a list's ids and
// their pages of details), yandexplay.c (the queue), yandexdl.c (save to the
// card), yandexsync.c (the likes), yandexwave.c (the station).
//
// The rule the old state screen had and this one keeps: opening the page never
// touches the network. Sections are read when they are opened, one request (two
// for the playlists, which are both the account's own and the ones the service
// makes) on a worker thread, paced by yandex.c's own throttle. A card with no
// keys, no token and no account still opens: it says so, and every row that
// needs the service answers with the sentence yandex.c gives it rather than
// waiting for anything.
//
// Memory: a list of five thousand liked tracks does not become five thousand
// widgets. The list screen has a pool of twelve rows (ROW_POOL) on a sheet as
// tall as the list, exactly the browser's windower (gui/library/medialist.c):
// a row object is moved to another index rather than made for it, and the
// details of a row are asked for by page (yandexlist.h), so the cost of a list
// follows the screen rather than the account.
//
// Threads: one job worker (lists, details, likes) and the page's own one-shot
// workers (a token, an OAuth code and its polls) -- all counted the way the
// addon's contract asks (yandexlive.h), so stop() can wait for them before the
// library is unmapped. gui_post() brings every answer back to the UI thread.
// ---------------------------------------------------------------------------

// One row of the list, the pool's size, and the caps of the pages that are not
// lists of ids. The row pool is a screenful and a half on this panel; the caps
// bound the result array a worker fills (a yandex_track_t is over a kilobyte).
#define MAX_ROWS 100
#define ARTIST_POPULAR 15
#define SEARCH_ARTISTS 20
#define SEARCH_ALBUMS 20
#define SEARCH_TRACKS 40
#define ROW_POOL 12

// One row of a list, and the gap the sheet puts between rows. Every row is
// exactly one pitch tall, which is what lets the windower turn a scroll offset
// into an index without measuring anything.
#define LIST_ROW_H SC(88)
#define LIST_ROW_GAP SC(10)
#define ROW_PITCH (LIST_ROW_H + LIST_ROW_GAP)
#define LIST_THUMB SC(60)
#define LETTER_BOX LIST_THUMB

typedef enum { ROW_TRACK, ROW_ALBUM, ROW_ARTIST, ROW_PLAYLIST, ROW_MORE } row_kind_t;

typedef struct {
	row_kind_t kind;
	int number; // ROW_MORE: how many entries behind it ("All tracks")
	union {
		yandex_track_t track;
		yandex_album_t album;
		yandex_artist_t artist;
		yandex_playlist_t playlist;
	} u;
} row_t;

typedef enum {
	JOB_NONE = 0,
	// lists
	JOB_LIKED,	JOB_ALBUMS,	 // liked albums
	JOB_ARTISTS, // liked artists
	JOB_PLAYLISTS,
	JOB_NEW_RELEASES,
	JOB_CHART,
	JOB_PLAYLIST, // `a`: owner, `b`: kind
	JOB_ALBUM,
	JOB_ARTIST,
	JOB_ARTIST_TRACKS, // all of an artist's tracks, as a paged list
	JOB_SEARCH,		   // `a`: the text
	// one page of the list on screen
	JOB_PAGE,
	// events: they answer a tap, they do not replace the list on screen
	JOB_WAVE,
	JOB_LIKE_ALBUM, // `n`: 1 like, 0 unlike
	JOB_LIKE_ARTIST,
	JOB_LIKES, // the mirror the star and the menus read, one request a session
	// built on the GUI thread from the index of saved tracks
	JOB_SAVED,
	JOB_SAVED_ALBUM,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char a[YANDEX_TITLE_MAX];
	char b[YANDEX_ID_MAX];
	int n;
	unsigned generation;
	yandexlist_t *list;
} job_t;

typedef struct {
	job_kind_t kind;
	unsigned generation;
	int page;
	int n;
	char id[YANDEX_ID_MAX];
	char title[YANDEX_TITLE_MAX]; // a page whose own answer names it (an album's)
	char error[512];
	yandexlist_t *list;
	int count;
	row_t rows[];
} job_result_t;

lv_obj_t *yandexstate_screen;

static lv_obj_t *list_screen, *search_screen;
static gui_config_t *page_cfg;
static char card_root[400]; // the card: the covers' cache (listart)
static char addon_dir[400]; // this addon's folder: token.txt, the session

// The id the host gave this library: every screen this page builds has to be
// registered with it as it is made (sonix_addon_own_screen, addon.h), or the
// shell leaves it on the display with callbacks into a library it is about to
// unmap. The manifest owns the string, so it is kept rather than copied.
static const char *own_id;

// What yandex.c's sentences say, for the ones the player has no language line
// for. An addon's strings are its own (docs/ADDONS.md, "Languages"): tr() comes
// back as the tag when the language file has no entry, and the player's file has
// none of the yandex_* tags. A language that does carry them returns a sentence
// that matches nothing here and is shown as it is, formatting and all.
static const char *say(const char *text) {
	static const struct {
		const char *tag;
		const char *sentence;
	} LINES[] = {
		{"yandex_not_signed_in", "Not signed in: sign in on the Yandex Music page."},
		{"yandex_session_expired", "The session has expired: sign in again."},
		{"yandex_too_many_requests", "The service is asking to slow down: try again in a minute."},
		{"yandex_no_reply", "The service did not answer."},
		{"yandex_unreadable_reply", "The service's answer could not be read."},
		{"yandex_answered_with", "The service answered with an error."},
		{"yandex_empty", "The service sent an empty file."},
		{"yandex_no_file", "The track's file could not be fetched."},
		{"yandex_cannot_decrypt", "The track's file could not be decrypted."},
		{"yandex_unsupported_file", "The track's file is in a form this player cannot play."},
		{"book_download_incomplete", "The book's list of chapters came incomplete."},
		{"book_download_limit", "The book has more chapters than this page reads."},
		{"svc_browse_unavailable", "This part of the catalogue is not available."},
	};
	if (!text || !text[0]) {
		return "";
	}
	for (size_t i = 0; i < sizeof(LINES) / sizeof(LINES[0]); i++) {
		if (strcmp(text, LINES[i].tag) == 0) {
			return LINES[i].sentence;
		}
	}
	return text;
}

// The covers of the list. Only its address matters (listrow.h gives `owner`
// the same meaning); one page owns the slots at a time.
static const char yandex_art_owner;

// The home page's widgets and the values they show. Declared here because the
// code flow (below) writes three of them from its timer, long before the home
// that creates them is rebuilt.
static lv_obj_t *home_container;
static lv_obj_t *quality_pills[YANDEX_QUALITY_COUNT];
static lv_obj_t *quality_note;
static lv_obj_t *account_value;
static lv_obj_t *keys_value;
static lv_obj_t *token_value;
static lv_obj_t *code_value;
static lv_obj_t *where_value;
static lv_obj_t *wait_value;
static bool wave_busy; // a batch is being asked for: the row does nothing meanwhile

// ---------------------------------------------------------------------------
// the page's threads, and the wait stop() does on them
//
// addon.h's contract: stop() may return only once no thread of the library is
// still running, because the player dlclose()s the mapping straight after. The
// addon counts its threads globally (yandexlive.h) and the page also counts its
// own, so that stop() frees this page's memory when *its* workers are gone
// rather than when the last download of the whole addon is.
// ---------------------------------------------------------------------------

static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t live_cond = PTHREAD_COND_INITIALIZER;
static int live_threads;

static void page_enter(void) {
	pthread_mutex_lock(&live_lock);
	live_threads++;
	pthread_mutex_unlock(&live_lock);
	yandexlive_enter(YANDEXLIVE_PAGE);
}

static void page_leave(void) {
	yandexlive_leave(YANDEXLIVE_PAGE);
	pthread_mutex_lock(&live_lock);
	if (live_threads > 0) {
		live_threads--;
	}
	if (live_threads == 0) {
		pthread_cond_broadcast(&live_cond);
	}
	pthread_mutex_unlock(&live_lock);
}

// True when every page thread has left. False is a thread still inside the
// library (a socket http.h cannot be woken out of), and the addon's stop hook
// turns that into "keep the mapping".
static bool page_wait(const char *what) {
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += YANDEXLIVE_WAIT_MS / 1000;
	deadline.tv_nsec += (long)(YANDEXLIVE_WAIT_MS % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&live_lock);
	while (live_threads > 0) {
		if (pthread_cond_timedwait(&live_cond, &live_lock, &deadline) != 0) {
			break;
		}
	}
	bool gone = live_threads == 0;
	int left = live_threads;
	pthread_mutex_unlock(&live_lock);
	if (!gone) {
		fprintf(stderr, "yandex: %s: %d thread%s still in the page after %d ms\n", what, left, left == 1 ? "" : "s",
				YANDEXLIVE_WAIT_MS);
	}
	return gone;
}

// ---------------------------------------------------------------------------
// the job worker
//
// One thread, one pending list job, one pending page fetch, and a small queue of
// events (My Wave, a like) that must not displace either. The shape is Zvuk's:
// a list job that is replaced by a newer one is a list nobody is looking at any
// more, but an event is one that was paid for by a tap and has to be answered.
// ---------------------------------------------------------------------------

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static job_t pending, page_pending;
#define EVENTS_MAX 8
static job_t events[EVENTS_MAX];
static int events_count;
static bool worker_started;
static bool stopping;
static unsigned list_generation;

// Worker side: the answer being built.
static row_t *result;
static int result_count;
static yandexlist_t *result_list;
static char result_error[512];
static char result_title[YANDEX_TITLE_MAX];

static void job_done_async(void *user);

static bool is_event(job_kind_t kind) {
	return kind == JOB_WAVE || kind == JOB_LIKE_ALBUM || kind == JOB_LIKE_ARTIST || kind == JOB_LIKES;
}

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	if (stopping) {
		// The worker is on its way out, and a job accepted now is one that never
		// runs: the page's reference to the list goes with it.
		pthread_mutex_unlock(&job_lock);
		if (job->kind == JOB_PAGE) {
			yandexlist_free(job->list);
		}
		return;
	}
	if (is_event(job->kind)) {
		if (events_count < EVENTS_MAX) {
			events[events_count++] = *job;
		}
	} else if (job->kind == JOB_PAGE) {
		yandexlist_free(page_pending.list);
		page_pending = *job;
	} else {
		pending = *job;
	}
	pthread_cond_signal(&job_cond);
	pthread_mutex_unlock(&job_lock);
}

static void add_id_cb(const char *track_id, const char *album_id, const yandex_track_t *track, void *user) {
	(void)track;
	if (track_id && track_id[0]) {
		yandexlist_add_id(user, track_id, album_id);
	}
}

static int list_from_tracks(const yandex_track_t *tracks, int got) {
	if (got < 0) {
		return got;
	}
	result_list = yandexlist_new();
	for (int i = 0; result_list && i < got; i++) {
		if (tracks[i].id[0]) {
			yandexlist_add_track(result_list, &tracks[i]);
		}
	}
	return result_list ? got : -1;
}

static int list_from_ids(int got, const char *what) {
	if (got < 0 || !result_list) {
		yandexlist_free(result_list);
		result_list = NULL;
		return -1;
	}
	if (got > 0 && yandexlist_fetch_page(result_list, 0) < 0) {
		// The ids are the queue; the first page is what the rows need. A failure
		// here leaves the list playable and every row saying "loading", so it is
		// logged and not turned into an error the page would show instead of the
		// list.
		fprintf(stderr, "yandex: %s: the first page did not come (%s)\n", what, yandex_last_error());
	}
	printf("yandex: %s: %d tracks, the list holds %zu bytes\n", what, got, yandexlist_memory(result_list));
	return got;
}

static void add_row(row_kind_t kind, const void *what) {
	if (result_count >= MAX_ROWS) {
		return;
	}
	row_t *r = &result[result_count++];
	memset(r, 0, sizeof(*r));
	r->kind = kind;
	switch (kind) {
	case ROW_TRACK:
		r->u.track = *(const yandex_track_t *)what;
		break;
	case ROW_ALBUM:
		r->u.album = *(const yandex_album_t *)what;
		break;
	case ROW_ARTIST:
		r->u.artist = *(const yandex_artist_t *)what;
		break;
	case ROW_PLAYLIST:
		r->u.playlist = *(const yandex_playlist_t *)what;
		break;
	case ROW_MORE:
		break;
	}
}

// The whole of a worker list job, one kind per arm. Every arm owns its
// allocations: a cap's worth of rows or tracks is allocated here and freed
// before the answer is built, never held for the life of the library.
static void run_list_job(const job_t *job) {
	switch (job->kind) {
	case JOB_LIKED:
		result_list = yandexlist_new();
		list_from_ids(result_list ? yandex_liked_ids(add_id_cb, result_list) : -1, "liked tracks");
		return;
	case JOB_PLAYLIST:
		result_list = yandexlist_new();
		list_from_ids(result_list ? yandex_playlist_ids(job->a, job->b, add_id_cb, result_list) : -1, "playlist");
		return;
	case JOB_CHART:
		result_list = yandexlist_new();
		list_from_ids(result_list ? yandex_chart(add_id_cb, result_list) : -1, "chart");
		return;
	case JOB_ARTIST_TRACKS:
		result_list = yandexlist_new();
		list_from_ids(result_list ? yandex_artist_track_ids(job->a, add_id_cb, result_list) : -1, "artist tracks");
		return;
	case JOB_ALBUM: {
		yandex_track_t *tracks = calloc(MAX_ROWS, sizeof(*tracks));
		yandex_album_meta_t meta;
		memset(&meta, 0, sizeof(meta));
		int got = tracks ? yandex_album_view(job->a, tracks, MAX_ROWS, &meta) : -1;
		if (got >= 0) {
			snprintf(result_title, sizeof(result_title), "%s", meta.title);
			got = list_from_tracks(tracks, got);
		}
		free(tracks);
		if (got < 0) {
			yandexlist_free(result_list);
			result_list = NULL;
			result_count = 0;
			snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
		}
		return;
	}
	default:
		break;
	}

	// The kinds that answer with rows. Each arm allocates the array it needs and
	// frees it before the answer is built: a hundred rows of a yandex_track_t
	// is 120 KB, held for the length of one job and no longer.
	int got = -1;
	switch (job->kind) {
	case JOB_ALBUMS:
	case JOB_NEW_RELEASES: {
		yandex_album_t *albums = calloc(MAX_ROWS, sizeof(*albums));
		if (!albums) {
			snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
			return;
		}
		got = job->kind == JOB_ALBUMS ? yandex_liked_albums(albums, MAX_ROWS) : yandex_new_releases(albums, MAX_ROWS);
		for (int i = 0; i < got; i++) {
			add_row(ROW_ALBUM, &albums[i]);
		}
		free(albums);
		break;
	}
	case JOB_ARTISTS: {
		yandex_artist_t *artists = calloc(MAX_ROWS, sizeof(*artists));
		if (!artists) {
			snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
			return;
		}
		got = yandex_liked_artists(artists, MAX_ROWS);
		for (int i = 0; i < got; i++) {
			add_row(ROW_ARTIST, &artists[i]);
		}
		free(artists);
		break;
	}
	case JOB_PLAYLISTS: {
		// The account's own, then the ones the service makes for it (Playlist
		// of the Day, Deja Vu...): two requests, because the API keeps them
		// apart, but one page, because the user does not.
		yandex_playlist_t *lists = calloc(MAX_ROWS / 2, sizeof(*lists));
		if (!lists) {
			snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
			return;
		}
		got = yandex_playlists(lists, MAX_ROWS / 2);
		for (int i = 0; i < got; i++) {
			add_row(ROW_PLAYLIST, &lists[i]);
		}
		if (got >= 0) {
			int more = yandex_feed_playlists(lists, MAX_ROWS / 2);
			if (more < 0) {
				fprintf(stderr, "yandex: playlists: the made-for-you list was not read (%s)\n", yandex_last_error());
			}
			for (int i = 0; i < more; i++) {
				add_row(ROW_PLAYLIST, &lists[i]);
			}
		}
		free(lists);
		break;
	}
	case JOB_ARTIST: {
		// Popular tracks and the records they are on, in that order: the two
		// requests the detail page needs, and the only ones it makes.
		yandex_track_t *tracks = calloc(ARTIST_POPULAR, sizeof(*tracks));
		if (!tracks) {
			snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
			return;
		}
		int total = 0;
		got = yandex_artist_tracks(job->a, tracks, ARTIST_POPULAR, &total);
		for (int i = 0; i < got; i++) {
			add_row(ROW_TRACK, &tracks[i]);
		}
		free(tracks);
		if (got >= 0) {
			yandex_album_t *albums = calloc(MAX_ROWS, sizeof(*albums));
			int na = albums ? yandex_artist_albums(job->a, albums, MAX_ROWS - result_count) : -1;
			if (na < 0) {
				fprintf(stderr, "yandex: artist albums: %s\n", yandex_last_error());
			}
			for (int i = 0; i < na; i++) {
				add_row(ROW_ALBUM, &albums[i]);
			}
			free(albums);
			if (total > ARTIST_POPULAR && result_count < MAX_ROWS) {
				add_row(ROW_MORE, NULL);
				result[result_count - 1].number = total;
			}
		}
		break;
	}
	case JOB_SEARCH: {
		// One /search call carries the three sections (yandex_search), so the
		// result page is one request: artists first, because from an artist the
		// rest of the catalogue is one tap away.
		yandex_artist_t *artists = calloc(SEARCH_ARTISTS, sizeof(*artists));
		yandex_album_t *albums = calloc(SEARCH_ALBUMS, sizeof(*albums));
		yandex_track_t *tracks = calloc(SEARCH_TRACKS, sizeof(*tracks));
		if (!artists || !albums || !tracks) {
			snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
		} else {
			int na = SEARCH_ARTISTS, nal = SEARCH_ALBUMS, nt = SEARCH_TRACKS;
			if (!yandex_search(job->a, artists, &na, albums, &nal, tracks, &nt)) {
				got = -1;
			} else {
				for (int i = 0; i < na; i++) {
					add_row(ROW_ARTIST, &artists[i]);
				}
				for (int i = 0; i < nal; i++) {
					add_row(ROW_ALBUM, &albums[i]);
				}
				for (int i = 0; i < nt; i++) {
					add_row(ROW_TRACK, &tracks[i]);
				}
				got = result_count;
				if (got == 0) {
					snprintf(result_error, sizeof(result_error), "%s", tr("no_results"));
				}
			}
		}
		free(artists);
		free(albums);
		free(tracks);
		break;
	}
	default:
		break;
	}
	if (got < 0) {
		yandexlist_free(result_list);
		result_list = NULL;
		result_count = 0;
		const char *why = yandex_last_error();
		snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : "the answer was not read");
	}
}

// My Wave: the first batch of a new station, turned into a list the UI thread
// can hand to yandexplay_list(). The station's later batches are yandexwave.c's
// business once the queue is playing (yandexplay_list(.., wave=true) begins it).
static void run_wave_job(void) {
	yandex_track_t *tracks = calloc(YANDEXWAVE_BATCH_MAX, sizeof(*tracks));
	if (!tracks) {
		snprintf(result_error, sizeof(result_error), "%s", tr("out_of_memory"));
		return;
	}
	int n = yandex_wave_start(NULL, 0, tracks, YANDEXWAVE_BATCH_MAX);
	if (n > 0) {
		result_list = yandexlist_new();
	}
	for (int i = 0; result_list && i < n; i++) {
		if (tracks[i].id[0]) {
			yandexlist_add_track(result_list, &tracks[i]);
		}
	}
	free(tracks);
	if (!result_list || yandexlist_count(result_list) == 0) {
		yandexlist_free(result_list);
		result_list = NULL;
		const char *why = yandex_last_error();
		snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : "the station said nothing");
	}
}

static job_result_t *job_result_new(const job_t *job) {
	int count = result_count;
	job_result_t *r = malloc(sizeof(*r) + (size_t)count * sizeof(row_t));
	if (!r) {
		yandexlist_free(result_list);
		result_list = NULL;
		return NULL;
	}
	memset(r, 0, sizeof(*r));
	r->kind = job->kind;
	r->generation = job->generation;
	r->page = job->n;
	r->n = job->n;
	snprintf(r->id, sizeof(r->id), "%.*s", YANDEX_ID_MAX - 1, job->a);
	snprintf(r->title, sizeof(r->title), "%s", result_title);
	snprintf(r->error, sizeof(r->error), "%s", result_error);
	r->list = result_list;
	result_list = NULL;
	r->count = count;
	if (count > 0) {
		memcpy(r->rows, result, (size_t)count * sizeof(row_t));
	}
	return r;
}

static void free_result(job_result_t *r) {
	if (r) {
		yandexlist_free(r->list);
		free(r);
	}
}

static void *job_worker(void *arg) {
	(void)arg;
	// Counted by start_job_worker() before this thread existed: a thread that
	// has been created but has not reached its first line must not slip past
	// the wait in yandexstate_stop(). It is uncounted on the way out.
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
		result_title[0] = '\0';
		result_count = 0;
		result_list = NULL;
		switch (job.kind) {
		case JOB_PAGE:
			// A page that fails is not the list's failure: the rows already
			// here stay, and the page is retried after a pause (request_page).
			if (yandexlist_fetch_page(job.list, job.n) < 0) {
				snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
			}
			result_list = job.list;
			job.list = NULL;
			break;
		case JOB_WAVE:
			run_wave_job();
			break;
		case JOB_LIKE_ALBUM:
			if (!yandex_like_album(job.a, job.n != 0)) {
				snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
			}
			break;
		case JOB_LIKE_ARTIST:
			if (!yandex_like_artist(job.a, job.n != 0)) {
				snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
			}
			break;
		case JOB_LIKES:
			// The mirror the like menus and the player's star read, read here
			// rather than by yandexsync's worker: a worker of another module
			// that is idle is still counted in the library (yandexlive.h), and
			// every module's stop() waits on that count -- so a page that starts
			// one for a read it could do itself is a page that keeps the addon
			// from ever being unloaded. One request a session; the Liked tracks
			// section fills the same mirror on the way.
			if (!yandex_liked_known() && !yandex_liked_refresh()) {
				snprintf(result_error, sizeof(result_error), "%s", yandex_last_error());
			}
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
	page_leave();
	return NULL;
}

static void start_job_worker(void) {
	if (worker_started || stopping) {
		return;
	}
	if (!result) {
		result = calloc(MAX_ROWS, sizeof(*result));
	}
	if (!result) {
		return;
	}
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024); // TLS, as the other workers
	// Counted before the thread exists and uncounted when it never started: a
	// thread that is created and has not reached its first line must not slip
	// past the wait in yandexstate_stop().
	page_enter();
	worker_started = pthread_create(&th, &attr, job_worker, NULL) == 0;
	if (!worker_started) {
		page_leave();
	}
	pthread_attr_destroy(&attr);
}

static void run(job_kind_t kind, const char *a, const char *b, int n) {
	start_job_worker();
	job_t job = {.kind = kind, .n = n, .generation = list_generation};
	snprintf(job.a, sizeof(job.a), "%s", a ? a : "");
	snprintf(job.b, sizeof(job.b), "%s", b ? b : "");
	submit(&job);
}

// The page's own one-shot workers: a token from the card, an OAuth code, a poll
// for it. One request each, no flag to be asked to stop -- they are waited for
// like the job worker, with the same bound.
typedef struct {
	void *(*main_fn)(void *);
} one_shot_t;

static void *one_shot_main(void *arg) {
	one_shot_t *w = arg;
	void *(*main_fn)(void *) = w->main_fn;
	free(w);
	// Counted by start_one_shot() before this thread existed; uncounted on the
	// way out.
	main_fn(NULL);
	page_leave();
	return NULL;
}

static void start_one_shot(void *(*main_fn)(void *)) {
	if (stopping) {
		return;
	}
	one_shot_t *w = malloc(sizeof(*w));
	if (!w) {
		gui_notify_popup("could not start a worker");
		return;
	}
	w->main_fn = main_fn;
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024); // TLS: as the other workers
	page_enter();
	if (pthread_create(&th, &attr, one_shot_main, w) != 0) {
		page_leave();
		free(w);
		gui_notify_popup("could not start a worker");
	}
	pthread_attr_destroy(&attr);
}

// ---------------------------------------------------------------------------
// what the list is showing
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *button;
	lv_obj_t *thumb; // the sleeve: listrow.h writes into it
	lv_obj_t *letter;
	lv_obj_t *letter_label;
	lv_obj_t *number;
	lv_obj_t *number_label;
	lv_obj_t *name;
	lv_obj_t *detail;
	lv_obj_t *menu;
	int index; // -1: the slot is not showing anything
	bool have; // the details of `index` are here
} row_view_t;

static lv_obj_t *list_view; // the scroller
static lv_obj_t *list_body; // the sheet as tall as the list, which holds the pool
static lv_obj_t *list_title;
static lv_obj_t *list_empty, *list_spin;
static row_view_t pool[ROW_POOL];
static job_kind_t list_kind;
static job_result_t *shown;
static int page_asked = -1, page_failed = -1;
static time_t page_failed_at;
static char playing_id[YANDEX_ID_MAX];
static bool autoplay_pending;
static lv_timer_t *play_timer;

static void rebuild_home(void);
static void list_render(void);
static void request_page(int page);
static void row_clicked_cb(lv_event_t *e);
static void row_menu_cb(lv_event_t *e);

static int view_count(void) {
	if (!shown) {
		return 0;
	}
	return shown->list ? yandexlist_count(shown->list) : shown->count;
}

// One entry of what is on screen. False while the entry's details are still on
// the way: `out` then says nothing but the kind.
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
	return yandexlist_track(shown->list, index, &out->u.track);
}

static bool row_saved(const row_t *r) {
	if (r->kind == ROW_TRACK) {
		return r->u.track.id[0] && yandexsaved_has(r->u.track.id);
	}
	if (r->kind == ROW_ALBUM && r->u.album.id[0] && r->u.album.track_count > 0) {
		return yandexsaved_album_count(r->u.album.id) >= r->u.album.track_count;
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
		snprintf(name, ns, "All tracks");
		if (r->number > 0) {
			snprintf(detail, ds, "%d tracks", r->number);
		} else {
			detail[0] = '\0';
		}
		break;
	case ROW_TRACK: {
		snprintf(name, ns, "%s", r->u.track.title);
		char length[16];
		duration_text(r->u.track.duration, length, sizeof(length));
		snprintf(detail, ds, "%s%s%s", r->u.track.artist, length[0] ? " \xC2\xB7 " : "", length);
		if (!r->u.track.available) {
			size_t used = strlen(detail);
			snprintf(detail + used, ds > used ? ds - used : 0, "%sUnavailable", used ? " \xC2\xB7 " : "");
		} else if (row_saved(r)) {
			size_t used = strlen(detail);
			snprintf(detail + used, ds > used ? ds - used : 0, "%sSaved", used ? " \xC2\xB7 " : "");
		}
		break;
	}
	case ROW_ALBUM:
		snprintf(name, ns, "%s", r->u.album.title);
		if (r->u.album.year) {
			snprintf(detail, ds, "%s \xC2\xB7 %d", r->u.album.artist, r->u.album.year);
		} else {
			snprintf(detail, ds, "%s", r->u.album.artist);
		}
		if (r->u.album.track_count > 0) {
			size_t used = strlen(detail);
			snprintf(detail + used, ds > used ? ds - used : 0, "%s%d tracks", used ? " \xC2\xB7 " : "",
					 r->u.album.track_count);
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
	char id[YANDEX_ID_MAX] = "";
	yandex_track_t current;
	if (yandexplay_current(&current)) {
		snprintf(id, sizeof(id), "%s", current.id);
	}
	if (strcmp(id, playing_id) == 0) {
		return false;
	}
	snprintf(playing_id, sizeof(playing_id), "%s", id);
	return true;
}

// ---------------------------------------------------------------------------
// a row of the pool
// ---------------------------------------------------------------------------

// Whether the row has anything in its menu at all.
static bool row_menu_available(const row_t *r) {
	switch (r->kind) {
	case ROW_TRACK:
		return r->u.track.id[0] != '\0';
	case ROW_ALBUM:
		return r->u.album.id[0] != '\0';
	case ROW_ARTIST:
		return r->u.artist.id[0] != '\0';
	case ROW_PLAYLIST:
		return r->u.playlist.kind[0] != '\0';
	default:
		return false;
	}
}

static lv_obj_t *make_lead_box(lv_obj_t *row) {
	lv_obj_t *box = lv_obj_create(row);
	lv_obj_set_size(box, LETTER_BOX, LETTER_BOX);
	lv_obj_set_style_bg_color(box, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_radius(box, SC(8), 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_event_bubble(box, true);
	return box;
}

// One row object, built once and then moved from index to index by list_band().
// Everything a row can show is created here and shown or hidden at bind time,
// because creating and deleting widgets as the user scrolls is what makes a
// list of thousands cost what its screen costs.
static void build_row_view(row_view_t *v) {
	v->index = -1;
	v->button = lv_btn_create(list_body);
	lv_obj_set_size(v->button, lv_pct(100), LIST_ROW_H);
	lv_obj_add_style(v->button, &theme_style_card, 0);
	lv_obj_add_style(v->button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(v->button, SC(12), 0);
	lv_obj_set_style_border_width(v->button, 0, 0);
	lv_obj_set_style_shadow_width(v->button, 0, 0);
	lv_obj_set_style_pad_hor(v->button, SC(14), 0);
	lv_obj_set_style_pad_ver(v->button, 0, 0);
	lv_obj_set_style_pad_column(v->button, SC(10), 0);
	lv_obj_set_scrollable(v->button, false);
	lv_obj_set_flex_flow(v->button, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(v->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// Gestures (back, and pulling in the player sheet) start on rows too, and a
	// button keeps the press to itself: without this neither drag works here.
	lv_obj_set_event_bubble(v->button, true);
	lv_obj_add_event_cb(v->button, row_clicked_cb, LV_EVENT_CLICKED, v);
	lv_obj_set_hidden(v->button, true);

	v->thumb = lv_image_create(v->button);
	lv_obj_set_size(v->thumb, LIST_THUMB, LIST_THUMB);
	lv_obj_set_style_bg_color(v->thumb, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_opa(v->thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(v->thumb, SC(8), 0);
	lv_obj_set_style_clip_corner(v->thumb, true, 0);
	lv_obj_set_event_bubble(v->thumb, true);

	v->letter = make_lead_box(v->button);
	v->letter_label = lv_label_create(v->letter);
	lv_obj_add_style(v->letter_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(v->letter_label, &font_ui_24, 0);
	lv_obj_center(v->letter_label);

	v->number = make_lead_box(v->button);
	lv_obj_set_style_bg_opa(v->number, LV_OPA_TRANSP, 0);
	v->number_label = lv_label_create(v->number);
	lv_obj_add_style(v->number_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(v->number_label, &font_ui_22, 0);
	lv_obj_center(v->number_label);

	lv_obj_t *texts = lv_obj_create(v->button);
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

	v->name = lv_label_create(texts);
	lv_label_set_text(v->name, "");
	lv_label_set_long_mode(v->name, LV_LABEL_LONG_DOT);
	lv_obj_set_width(v->name, lv_pct(100));
	lv_obj_set_height(v->name, lv_font_get_line_height(&font_ui_22));
	lv_obj_add_style(v->name, &theme_style_text, 0);
	lv_obj_set_style_text_font(v->name, &font_ui_22, 0);

	v->detail = lv_label_create(texts);
	lv_label_set_text(v->detail, "");
	lv_label_set_long_mode(v->detail, LV_LABEL_LONG_DOT);
	lv_obj_set_width(v->detail, lv_pct(100));
	lv_obj_set_height(v->detail, lv_font_get_line_height(&font_ui_18));
	lv_obj_add_style(v->detail, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(v->detail, &font_ui_18, 0);

	v->menu = lv_btn_create(v->button);
	lv_obj_set_size(v->menu, SC(44), SC(44));
	lv_obj_set_style_bg_opa(v->menu, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(v->menu, 0, 0);
	lv_obj_set_style_shadow_width(v->menu, 0, 0);
	lv_obj_set_style_pad_all(v->menu, 0, 0);
	lv_obj_add_event_cb(v->menu, row_menu_cb, LV_EVENT_CLICKED, v);
	lv_obj_t *dots = lv_image_create(v->menu);
	lv_image_set_src(dots, &icon_ellipsis_vertical);
	lv_obj_add_style(dots, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
	lv_obj_center(dots);
}

static void show_lead(row_view_t *v, lv_obj_t *lead, bool show) { lv_obj_set_hidden(lead, !show); }

// What sits to the left of a row's texts: a sleeve, a letter, or the track's
// place on the record, and nothing at all while the details are on the way.
static void bind_lead(row_view_t *v, const row_t *r, int index) {
	bool have = v->have;
	bool album_list = list_kind == JOB_ALBUM || list_kind == JOB_SAVED_ALBUM;
	bool thumb = false, letter = false, number = false;
	if (have) {
		switch (r->kind) {
		case ROW_TRACK:
			if (album_list && r->u.track.track_number > 0) {
				number = true;
				lv_label_set_text_fmt(v->number_label, "%d", r->u.track.track_number);
			} else if (r->u.track.cover[0]) {
				thumb = true;
			}
			break;
		case ROW_ALBUM:
			thumb = r->u.album.cover[0] != '\0';
			break;
		case ROW_ARTIST:
			letter = true;
			break;
		case ROW_PLAYLIST:
			letter = true;
			break;
		default:
			break;
		}
	}
	if (letter) {
		char glyph[8];
		first_char(r->kind == ROW_ARTIST ? r->u.artist.name : r->u.playlist.title, glyph, sizeof(glyph));
		lv_label_set_text(v->letter_label, glyph);
	}
	show_lead(v, v->thumb, thumb);
	show_lead(v, v->letter, letter);
	show_lead(v, v->number, number);
	if (thumb) {
		char url[YANDEX_URL_MAX];
		const char *cover = r->kind == ROW_TRACK ? r->u.track.cover : r->u.album.cover;
		yandex_cover_url(cover, 100, url, sizeof(url));
		listrow_bind((void *)&yandex_art_owner, v->thumb, index, url);
	} else {
		listrow_bind((void *)&yandex_art_owner, v->thumb, index, NULL);
	}
}

static void bind_row_view(row_view_t *v, int index) {
	row_t r;
	bool have = view_row(index, &r);
	v->index = index;
	v->have = have;

	char name[YANDEX_TITLE_MAX], detail[YANDEX_TITLE_MAX + 48];
	if (!have) {
		// The list is here, the details are not yet: the row says so instead of
		// vanishing, so the list does not jump when the page arrives.
		snprintf(name, sizeof(name), "%s", tr("loading"));
		detail[0] = '\0';
		r.kind = ROW_TRACK;
	} else {
		row_texts(&r, name, sizeof(name), detail, sizeof(detail));
	}
	lv_label_set_text(v->name, name);
	lv_label_set_text(v->detail, detail);
	lv_obj_set_hidden(v->detail, detail[0] == '\0');

	bind_lead(v, &r, index);

	// What is playing, marked the way the player marks it: the accent colour.
	bool playing = have && r.kind == ROW_TRACK && playing_id[0] && strcmp(playing_id, r.u.track.id) == 0;
	if (playing) {
		lv_obj_set_style_text_color(v->name, theme()->accent, 0);
	} else {
		lv_obj_remove_local_style_prop(v->name, LV_STYLE_TEXT_COLOR, 0);
	}

	lv_obj_set_hidden(v->menu, !(have && row_menu_available(&r)));
	lv_obj_set_y(v->button, index * ROW_PITCH);
	lv_obj_set_x(v->button, 0);
	lv_obj_set_hidden(v->button, false);
}

// Which rows the pool is showing: the visible ones and one above and below, so
// a flick has something to draw before the next scroll event rebinds.
static void list_band(void) {
	if (!list_body) {
		return;
	}
	int count = view_count();
	if (count <= 0) {
		for (int i = 0; i < ROW_POOL; i++) {
			pool[i].index = -1;
			pool[i].have = false;
			lv_obj_set_hidden(pool[i].button, true);
			listrow_bind((void *)&yandex_art_owner, pool[i].thumb, i, NULL);
		}
		listrow_visible((void *)&yandex_art_owner, 0, -1);
		return;
	}
	int top = lv_obj_get_scroll_y(list_view);
	int height = lv_obj_get_height(list_view);
	if (height <= 0) {
		height = (int)page_cfg->screen_height;
	}
	if (top < 0) {
		top = 0;
	}
	int visible_first = top / ROW_PITCH;
	int visible_last = (top + height - 1) / ROW_PITCH;
	if (visible_first >= count) {
		visible_first = count - 1;
	}
	if (visible_last >= count) {
		visible_last = count - 1;
	}
	int band = visible_first > 1 ? visible_first - 1 : 0;

	bool used[ROW_POOL] = {false};
	for (int i = 0; i < ROW_POOL; i++) {
		int index = band + i;
		if (index >= count) {
			break;
		}
		row_view_t *v = &pool[index % ROW_POOL];
		bind_row_view(v, index);
		used[index % ROW_POOL] = true;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		if (!used[i]) {
			pool[i].index = -1;
			pool[i].have = false;
			lv_obj_set_hidden(pool[i].button, true);
			listrow_bind((void *)&yandex_art_owner, pool[i].thumb, i, NULL);
		}
	}

	listrow_visible((void *)&yandex_art_owner, visible_first, visible_last);

	// The pages the rows need: the visible band and the pages either side of it,
	// a page at a time (yandexlist.h fetches fifty entries per request).
	int pages[3] = {visible_first / YANDEXLIST_PAGE, visible_last / YANDEXLIST_PAGE,
					(visible_last + YANDEXLIST_PAGE / 2) / YANDEXLIST_PAGE};
	for (int i = 0; i < 3; i++) {
		if (shown && shown->list && pages[i] * YANDEXLIST_PAGE < view_count() &&
			!yandexlist_page_ready(shown->list, pages[i])) {
			request_page(pages[i]);
			break;
		}
	}
}

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	list_band();
}

static void list_render(void) {
	if (!list_body) {
		return;
	}
	int count = view_count();
	// The sheet is as tall as the whole list; the pool inside it is what on
	// screen. That is what makes the scrollbar honest without a widget per row.
	int height = count * ROW_PITCH;
	int view_h = lv_obj_get_height(list_view);
	lv_obj_set_height(list_body, height > view_h ? height : (view_h > 0 ? view_h : ROW_PITCH));

	if (shown && shown->title[0]) {
		lv_label_set_text(list_title, shown->title);
	}

	if (shown && count == 0) {
		const char *text = shown->error[0] ? say(shown->error) : tr("no_results");
		lv_label_set_text(list_empty, text);
		lv_obj_set_hidden(list_empty, false);
	} else {
		lv_obj_set_hidden(list_empty, true);
	}
	list_band();
}

// ---------------------------------------------------------------------------
// paging
// ---------------------------------------------------------------------------

static void request_page(int page) {
	if (!shown || !shown->list || page == page_asked || (page == page_failed && time(NULL) - page_failed_at < 5)) {
		return;
	}
	page_asked = page;
	start_job_worker();
	job_t job = {.kind = JOB_PAGE, .n = page, .generation = list_generation, .list = yandexlist_ref(shown->list)};
	submit(&job);
}

// ---------------------------------------------------------------------------
// the list page's own history
//
// The screen manager sees one list screen, so opening an album from the results
// and going back would leave the page. The page keeps a stack of steps instead:
// the chevron consumes one before it lets the page close.
// ---------------------------------------------------------------------------

typedef struct {
	job_kind_t kind;
	char title[YANDEX_TITLE_MAX];
	char a[YANDEX_TITLE_MAX];
	char b[YANDEX_ID_MAX];
	int n;
	bool autoplay;
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

static void open_list(const char *title, job_kind_t kind, const char *a, const char *b, int n, bool autoplay) {
	list_step_t step;
	memset(&step, 0, sizeof(step));
	step.kind = kind;
	step.n = n;
	step.autoplay = autoplay;
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
	yandex_track_t *tracks = calloc(MAX_ROWS, sizeof(*tracks));
	if (!r || !tracks) {
		free(r);
		free(tracks);
		return NULL;
	}
	r->kind = kind;
	r->generation = list_generation;
	int count = 0;
	if (kind == JOB_SAVED) {
		yandex_album_t *albums = (yandex_album_t *)tracks;
		int n = yandexsaved_albums(albums, MAX_ROWS / 2);
		for (int i = 0; i < n; i++) {
			r->rows[count].kind = ROW_ALBUM;
			r->rows[count++].u.album = albums[i];
		}
		n = yandexsaved_tracks(NULL, tracks, MAX_ROWS - count);
		for (int i = 0; i < n; i++) {
			r->rows[count].kind = ROW_TRACK;
			r->rows[count++].u.track = tracks[i];
		}
	} else {
		int n = yandexsaved_tracks(album_id, tracks, MAX_ROWS);
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
	if (list_title && step->title[0]) {
		lv_label_set_text(list_title, step->title);
	}
	lv_obj_set_hidden(list_empty, true);
	lv_obj_set_hidden(list_spin, true);
	page_asked = -1;
	page_failed = -1;
	autoplay_pending = step->autoplay;
	if (lv_screen_active() != list_screen) {
		switch_screen(list_screen);
	}
	lv_obj_scroll_to_y(list_view, 0, LV_ANIM_OFF);
	list_generation++;
	free_result(shown);
	shown = NULL;
	// A new list: every cover this page asked for goes with the rows that are
	// about to show another list's entries.
	listrow_reset((void *)&yandex_art_owner);
	list_render();
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

static void playback_event(yandexplay_event_t event, const char *text) {
	if (list_spin) {
		lv_obj_set_hidden(list_spin, true);
	}
	switch (event) {
	case YANDEXPLAY_STARTED:
		player_refresh_now_playing();
		if (playing_update()) {
			list_render();
		}
		break;
	case YANDEXPLAY_FAILED:
		toast_error(say(text && text[0] ? text : tr("download_failed")));
		break;
	case YANDEXPLAY_SKIPPED: {
		char line[320];
		snprintf(line, sizeof(line), "Skipped: %s", say(text));
		toast_error(line);
		break;
	}
	}
}

// The track a row would play, and whether the page has its details: a row whose
// page has not arrived yet still plays, because the list knows its ids and
// yandexplay fetches what it is missing.
static void play_rows(int tapped) {
	row_t r;
	bool have = view_row(tapped, &r);
	if (have && r.kind == ROW_TRACK && !r.u.track.available) {
		toast_error("This track cannot be played.");
		return;
	}
	lv_obj_set_hidden(list_spin, false);
	if (shown && shown->list) {
		if (!yandexplay_list(shown->list, tapped, false)) {
			lv_obj_set_hidden(list_spin, true);
		}
		return;
	}
	// A list of mixed rows (an artist's page, a search): the queue is the
	// playable tracks among them, in the order they are shown. Tapping a row
	// that is not a track (the corner button's "play all", or a row whose
	// details mislead) starts at the first track of the page.
	yandexlist_t *tracks = yandexlist_new();
	int start = -1;
	int count = view_count();
	for (int i = 0; tracks && i < count; i++) {
		row_t row;
		if (view_row(i, &row) && row.kind == ROW_TRACK && row.u.track.available &&
			yandexlist_add_track(tracks, &row.u.track) && i == tapped) {
			start = yandexlist_count(tracks) - 1;
		}
	}
	if (start < 0 && yandexlist_count(tracks) > 0) {
		start = 0;
	}
	if (start < 0 || !yandexplay_list(tracks, start, false)) {
		lv_obj_set_hidden(list_spin, true);
	}
	yandexlist_free(tracks);
}

// "Play" on a list that is not a track list: the whole of what is on screen,
// from the top. For a list of ids (the liked tracks) it is the same queue
// playback would build by tapping the first row; for a page of rows it is the
// tracks among them.
static void play_all(void) {
	if (!shown) {
		toast_plain("Nothing to play yet.");
		return;
	}
	if (shown->list) {
		if (yandexlist_count(shown->list) == 0) {
			toast_plain("Nothing to play yet.");
			return;
		}
		lv_obj_set_hidden(list_spin, false);
		if (!yandexplay_list(shown->list, 0, false)) {
			lv_obj_set_hidden(list_spin, true);
		}
		return;
	}
	play_rows(0);
}

static void row_clicked_cb(lv_event_t *e) {
	// A gesture that ends over a row is not a tap on it: events bubble.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	row_view_t *v = lv_event_get_user_data(e);
	if (!v || v->index < 0 || v->index >= view_count()) {
		return;
	}
	int index = v->index;
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
		// The artist's whole catalogue, as a paged list: the popular tracks on
		// this page are one request's worth of them.
		open_list(list_current.title, JOB_ARTIST_TRACKS, list_current.a, NULL, 0, false);
		break;
	case ROW_ALBUM:
		open_list(r.u.album.title, list_kind == JOB_SAVED ? JOB_SAVED_ALBUM : JOB_ALBUM, r.u.album.id, NULL, 0,
				  false);
		break;
	case ROW_ARTIST:
		open_list(r.u.artist.name, JOB_ARTIST, r.u.artist.id, NULL, 0, false);
		break;
	case ROW_PLAYLIST:
		open_list(r.u.playlist.title, JOB_PLAYLIST, r.u.playlist.owner, r.u.playlist.kind, 0, false);
		break;
	}
}

// ---------------------------------------------------------------------------
// the row menu
//
// The actions the APIs above support, and no queue of the fork's kind: "Play
// next" is the player's own playlist_insert_next(), fed the path yandexplay
// guesses for a track it is told about (yandexplay_paths_for), which is how a
// service adds to the queue without owning it.
// ---------------------------------------------------------------------------

static row_t menu_row;
static int menu_index;
static row_t delete_row;

static void refresh_after_change(void) {
	if (lv_screen_active() == list_screen) {
		list_render();
		rebuild_home();
	}
}

static void do_play(void *user) {
	(void)user;
	play_rows(menu_index);
}

static void do_play_next(void *user) {
	(void)user;
	if (menu_row.kind != ROW_TRACK || !menu_row.u.track.id[0]) {
		return;
	}
	char **paths = NULL;
	int n = yandexplay_paths_for(&menu_row.u.track, 1, &paths);
	bool ok = n > 0 && paths && paths[0] && playlist_insert_next(paths[0]);
	free(paths);
	if (ok) {
		toast_success("Added to the queue.");
	} else {
		toast_error("The track was not queued.");
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
		yandexsync_like(menu_row.u.track.id, menu_row.u.track.album_id, want, like_done, NULL);
		refresh_after_change();
		break;
	case ROW_ALBUM:
		run(JOB_LIKE_ALBUM, menu_row.u.album.id, NULL, want ? 1 : 0);
		break;
	case ROW_ARTIST:
		run(JOB_LIKE_ARTIST, menu_row.u.artist.id, NULL, want ? 1 : 0);
		break;
	default:
		break;
	}
}

static void save_refused(void) { toast_error("Nothing was saved: the card or the session refused."); }

static void do_save(void *user) {
	(void)user;
	bool ok = false;
	switch (menu_row.kind) {
	case ROW_TRACK:
		ok = yandexdl_track(&menu_row.u.track);
		break;
	case ROW_ALBUM:
		ok = yandexdl_album(menu_row.u.album.id, menu_row.u.album.title);
		break;
	default:
		break;
	}
	if (!ok) {
		save_refused();
	}
}

static void delete_confirmed(void *user) {
	(void)user;
	bool ok;
	if (delete_row.kind == ROW_ALBUM) {
		ok = yandexsaved_delete_album(delete_row.u.album.id) > 0 ||
			 yandexsaved_album_count(delete_row.u.album.id) == 0;
	} else {
		ok = yandexsaved_delete(delete_row.u.track.id);
	}
	if (ok) {
		toast_success("Deleted from the card.");
	} else {
		toast_error("Nothing was deleted.");
	}
	refresh_after_change();
}

static void do_delete(void *user) {
	(void)user;
	delete_row = menu_row;
	const char *name = menu_row.kind == ROW_ALBUM ? menu_row.u.album.title : menu_row.u.track.title;
	confirm_show("Delete from the card?", name, "delete", delete_confirmed, NULL);
}

static void do_open_artist(void *user) {
	(void)user;
	const char *id = menu_row.kind == ROW_TRACK ? menu_row.u.track.artist_id
					 : menu_row.kind == ROW_ALBUM ? menu_row.u.album.artist_id
												  : "";
	const char *name = menu_row.kind == ROW_TRACK ? menu_row.u.track.artist
					   : menu_row.kind == ROW_ALBUM ? menu_row.u.album.artist
													: "";
	if (id[0]) {
		open_list(name, JOB_ARTIST, id, NULL, 0, false);
	}
}

static void do_open_album(void *user) {
	(void)user;
	if (menu_row.kind == ROW_TRACK && menu_row.u.track.album_id[0]) {
		open_list(menu_row.u.track.album, JOB_ALBUM, menu_row.u.track.album_id, NULL, 0, false);
	}
}

static void do_play_page(void *user) {
	(void)user;
	switch (menu_row.kind) {
	case ROW_ALBUM:
		open_list(menu_row.u.album.title, list_kind == JOB_SAVED ? JOB_SAVED_ALBUM : JOB_ALBUM, menu_row.u.album.id,
				  NULL, 0, true);
		break;
	case ROW_PLAYLIST:
		open_list(menu_row.u.playlist.title, JOB_PLAYLIST, menu_row.u.playlist.owner, menu_row.u.playlist.kind, 0,
				  true);
		break;
	case ROW_ARTIST:
		open_list(menu_row.u.artist.name, JOB_ARTIST, menu_row.u.artist.id, NULL, 0, true);
		break;
	default:
		break;
	}
}

static void row_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e); // the tap belongs to the menu, not to the row
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	row_view_t *v = lv_event_get_user_data(e);
	if (!v || v->index < 0 || !view_row(v->index, &menu_row)) {
		return;
	}
	menu_index = v->index;

	popover_item_t items[7];
	memset(items, 0, sizeof(items));
	int n = 0;
	bool in = yandex_logged_in();

	switch (menu_row.kind) {
	case ROW_TRACK: {
		items[n++] = (popover_item_t){.label = "Play", .action = do_play};
		items[n++] = (popover_item_t){.label = "Play next", .action = do_play_next};
		bool saved = row_saved(&menu_row);
		if (saved) {
			items[n++] = (popover_item_t){.label = "Delete from the card", .action = do_delete};
		} else if (menu_row.u.track.available) {
			items[n++] = (popover_item_t){.label = "Save to the card", .action = do_save};
		}
		if (in) {
			bool liked = yandex_is_liked(menu_row.u.track.id);
			items[n++] = (popover_item_t){.label = liked ? "remove_from_favourites" : "medialist_add_to_favourites",
										  .action = do_like,
										  .user = liked ? NULL : (void *)1};
		}
		if (menu_row.u.track.artist_id[0]) {
			items[n++] = (popover_item_t){.label = "Open artist", .action = do_open_artist};
		}
		if (menu_row.u.track.album_id[0]) {
			items[n++] = (popover_item_t){.label = "Open album", .action = do_open_album};
		}
		break;
	}
	case ROW_ALBUM: {
		items[n++] = (popover_item_t){.label = "Play", .action = do_play_page};
		int saved = menu_row.u.album.id[0] ? yandexsaved_album_count(menu_row.u.album.id) : 0;
		if (saved > 0) {
			items[n++] = (popover_item_t){.label = "Delete from the card", .action = do_delete};
		} else {
			items[n++] = (popover_item_t){.label = "Save to the card", .action = do_save};
		}
		if (in) {
			bool liked = yandex_album_liked(menu_row.u.album.id);
			items[n++] = (popover_item_t){.label = liked ? "remove_from_favourites" : "medialist_add_to_favourites",
										  .action = do_like,
										  .user = liked ? NULL : (void *)1};
		}
		if (menu_row.u.album.artist_id[0]) {
			items[n++] = (popover_item_t){.label = "Open artist", .action = do_open_artist};
		}
		break;
	}
	case ROW_ARTIST:
		items[n++] = (popover_item_t){.label = "Play", .action = do_play_page};
		if (in) {
			bool liked = yandex_artist_liked(menu_row.u.artist.id);
			items[n++] = (popover_item_t){.label = liked ? "remove_from_favourites" : "medialist_add_to_favourites",
										  .action = do_like,
										  .user = liked ? NULL : (void *)1};
		}
		break;
	case ROW_PLAYLIST:
		items[n++] = (popover_item_t){.label = "Play", .action = do_play_page};
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
	// rebuilt -- only the band is told again (the covers of a row that scrolled
	// away were given back) and the playing row is repainted if the track
	// changed while the page was away.
	if (playing_update()) {
		list_render();
		return;
	}
	list_band();
}

static void play_all_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	play_all();
}

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	// The shell knows the screen init() returns and nothing else unless it is
	// told: this one it has to take off the display and delete at unload.
	sonix_addon_own_screen(own_id, list_screen);
	lv_obj_add_style(list_screen, &theme_style_screen, 0);
	int top = settingsrow_content_top(cfg);
	list_title = settingsrow_title(list_screen, cfg, "Yandex Music");
	// One corner button, the whole list from its first row: the liked tracks
	// play without opening a track first, as the state screen's second button
	// did.
	settingsrow_title_corner_slots(list_title, cfg, 1);
	lv_obj_t *play_btn = lv_btn_create(list_screen);
	lv_obj_set_size(play_btn, SC(56), SC(56));
	lv_obj_set_style_bg_opa(play_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(play_btn, 0, 0);
	lv_obj_set_style_shadow_width(play_btn, 0, 0);
	lv_obj_set_style_pad_all(play_btn, 0, 0);
	lv_obj_align(play_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(play_btn, play_all_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *play_icon = lv_image_create(play_btn);
	lv_image_set_src(play_icon, &icon_circle_play);
	lv_obj_add_style(play_icon, &theme_style_icon, 0);
	lv_obj_center(play_icon);

	// The viewport, then the sheet it scrolls. The rows are children of the
	// sheet, positioned by index, not of the viewport: that is the windower
	// (see the comment at the top of this file).
	list_view = lv_obj_create(list_screen);
	lv_obj_set_size(list_view, cfg->screen_width, cfg->screen_height - top);
	lv_obj_align(list_view, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(list_view, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(list_view, 0, 0);
	lv_obj_set_style_radius(list_view, 0, 0);
	lv_obj_set_style_pad_hor(list_view, cfg->padding, 0);
	lv_obj_set_style_pad_ver(list_view, 0, 0);
	lv_obj_set_scroll_dir(list_view, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list_view, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(list_view, list_scrolled_cb, LV_EVENT_SCROLL, NULL);

	list_body = lv_obj_create(list_view);
	lv_obj_set_width(list_body, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_height(list_body, ROW_PITCH);
	lv_obj_set_pos(list_body, 0, 0);
	lv_obj_set_style_bg_opa(list_body, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(list_body, 0, 0);
	lv_obj_set_style_pad_all(list_body, 0, 0);
	lv_obj_set_scrollable(list_body, false);
	lv_obj_set_event_bubble(list_body, true);

	for (int i = 0; i < ROW_POOL; i++) {
		build_row_view(&pool[i]);
	}

	list_empty = lv_label_create(list_screen);
	lv_label_set_text(list_empty, tr("no_results"));
	lv_label_set_long_mode(list_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(list_empty, cfg->screen_width - 2 * cfg->padding - SC(12));
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_22, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, top + SC(100));
	lv_obj_set_hidden(list_empty, true);

	list_spin = spinner_create(list_screen, &icon_loader_big);
	lv_obj_align(list_spin, LV_ALIGN_TOP_MID, 0, top + SC(40));
	lv_obj_set_hidden(list_spin, true);

	lv_obj_add_event_cb(list_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(list_screen);
	switcher_attach_back_gesture(list_view);
	switcher_set_back_guard(list_screen, list_back_guard);
	switcher_set_back_guard_peek(list_screen, list_back_has_step);
	player_sheet_attach_drag(list_screen, true);
	player_sheet_attach_drag(list_view, true);
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

static lv_obj_t *search_field;
static keyboard_t *search_keyboard;

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !text[0]) {
		return;
	}
	char album_id[YANDEX_ID_MAX];
	// A copied album link opens the record it names without a search request.
	if (yandex_album_link_id(text, album_id, sizeof(album_id))) {
		open_list("Album", JOB_ALBUM, album_id, NULL, 0, false);
		return;
	}
	open_list(text, JOB_SEARCH, text, NULL, 0, false);
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (!yandex_logged_in()) {
		gui_notify_popup("Sign in first: search needs the account.");
		return;
	}
	switch_screen(search_screen);
}

static void build_search_page(gui_config_t *cfg) {
	search_screen = lv_obj_create(NULL);
	// Registered for the same reason as the list screen (see build_list_page).
	sonix_addon_own_screen(own_id, search_screen);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, cfg, "Search");
	int top = settingsrow_content_top(cfg);

	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, "Artist, album or track");
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

	search_keyboard = keyboard_create(search_screen, cfg->screen_width, SC(316), search_field, NULL, "ok",
									  search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
}

// ---------------------------------------------------------------------------
// the token on the card, the account, and the code flow
// ---------------------------------------------------------------------------

static yandex_login_t login;
static lv_timer_t *login_timer;
static int login_elapsed;  // seconds since the code arrived
static bool login_active;  // the page is waiting for a code to be typed
static bool login_polling; // a poll is in flight: never two at once

static void login_clear(void) {
	if (code_value) {
		lv_label_set_text(code_value, "");
	}
	if (where_value) {
		lv_label_set_text(where_value, "");
	}
	if (wait_value) {
		lv_label_set_text(wait_value, "");
	}
}

static void login_stop(void) {
	login_active = false;
	login_polling = false;
	if (login_timer) {
		lv_timer_delete(login_timer);
		login_timer = NULL;
	}
}

// The token sits beside the addon's settings, in its own folder on the card:
// nothing this addon needs is at the card's root.
static void token_path(char *out, size_t size) {
	if (!addon_dir[0]) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%s/%s", addon_dir, YANDEX_TOKEN_FILE);
}

// The first line of the token file that has anything on it. A whole
// "Authorization: OAuth ..." line pasted in is one line too, and
// yandex_import_token() takes the token out of it.
static bool read_token_file(char *out, size_t size) {
	char path[520];
	out[0] = '\0';
	token_path(path, sizeof(path));
	FILE *f = path[0] ? fopen(path, "r") : NULL;
	if (!f) {
		return false;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		const char *start = line;
		// A UTF-8 BOM, as an editor on Windows leaves it.
		if ((unsigned char)start[0] == 0xef && (unsigned char)start[1] == 0xbb && (unsigned char)start[2] == 0xbf) {
			start += 3;
		}
		while (*start == ' ' || *start == '\t') {
			start++;
		}
		if (*start) {
			snprintf(out, size, "%s", start);
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

static void home_values(void);

typedef struct {
	bool ok;
	char message[256];
} answer_t;

static void answer_async(void *user) {
	answer_t *a = user;
	if (a->message[0]) {
		gui_notify_popup(say(a->message));
	}
	home_values();
	rebuild_home();
	free(a);
}

static void *sign_in_main(void *arg) {
	(void)arg;
	answer_t *a = calloc(1, sizeof(*a));
	if (!a) {
		return NULL;
	}
	char token[1024];
	if (!read_token_file(token, sizeof(token))) {
		// The user dropped the file where it used to go, or nowhere: name the
		// path this build reads, not the card's root.
		snprintf(a->message, sizeof(a->message), "no token.txt in the addon's folder (Addons/%s/)",
				 own_id ? own_id : "yandex");
		gui_post(answer_async, a);
		return NULL;
	}
	a->ok = yandex_import_token(token, a->message, sizeof(a->message));
	memset(token, 0, sizeof(token));
	if (!a->message[0]) {
		snprintf(a->message, sizeof(a->message), "%s", a->ok ? "signed in" : "the token was not accepted");
	}
	if (!gui_post(answer_async, a)) {
		free(a);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// signing in with a code (OAuth device flow)
//
// The backend has been in yandex.c from the start: yandex_login_begin() asks
// oauth.yandex.ru for a code and yandex_login_poll() trades it for tokens,
// saving the session and fetching the account on the way. The polls are made by
// a worker one at a time, paced by an LVGL timer on the GUI thread: a
// sleep-and-retry loop inside one job would hold the worker for the whole life
// of the code, which is minutes. Polling stops when the page is left, or it
// would be one request every few seconds forever.
// ---------------------------------------------------------------------------

typedef struct {
	bool ok;
	char message[256];
} code_answer_t;

typedef struct {
	yandex_login_state_t state;
	char message[256];
} poll_answer_t;

static void code_async(void *user);
static void poll_async(void *user);

static void *code_main(void *arg) {
	(void)arg;
	code_answer_t *a = calloc(1, sizeof(*a));
	if (!a) {
		return NULL;
	}
	if (!yandex_login_begin(&login)) {
		const char *why = yandex_last_error();
		snprintf(a->message, sizeof(a->message), "%s", why && why[0] ? why : "the code was not given out");
	} else {
		a->ok = true;
	}
	if (!gui_post(code_async, a)) {
		free(a);
	}
	return NULL;
}

static void *poll_main(void *arg) {
	(void)arg;
	poll_answer_t *a = calloc(1, sizeof(*a));
	if (!a) {
		return NULL;
	}
	a->state = yandex_login_poll(&login);
	if (a->state == YANDEX_LOGIN_ERROR) {
		const char *why = yandex_last_error();
		snprintf(a->message, sizeof(a->message), "%s", why && why[0] ? why : "sign in failed");
	}
	if (!gui_post(poll_async, a)) {
		free(a);
	}
	return NULL;
}

static void login_ended(const char *message) {
	login_stop();
	login_clear();
	if (message && message[0]) {
		gui_notify_popup(message);
	}
}

static void login_tick_cb(lv_timer_t *timer) {
	(void)timer;
	if (!login_active) {
		return;
	}
	login_elapsed++;

	// The code has a lifetime after which the service stops recognising it;
	// polling past that point is pure traffic.
	if (login.expires_secs > 0 && login_elapsed >= login.expires_secs) {
		login_ended("the code has expired");
		return;
	}

	char left[64];
	if (login.expires_secs > 0) {
		snprintf(left, sizeof(left), "waiting (%ds left)", login.expires_secs - login_elapsed);
	} else {
		snprintf(left, sizeof(left), "waiting (%ds)", login_elapsed);
	}
	if (wait_value) {
		lv_label_set_text(wait_value, left);
	}

	int interval = login.interval_secs > 0 ? login.interval_secs : 5;
	if (login_elapsed % interval != 0 || login_polling) {
		return;
	}
	login_polling = true;
	start_one_shot(poll_main);
}

// The code arrived: show it, and start the timer that paces the polls.
static void code_async(void *user) {
	code_answer_t *a = user;
	if (!login_active) {
		free(a); // the page stopped waiting meanwhile
		return;
	}
	if (!a->ok) {
		login_ended(say(a->message));
		free(a);
		return;
	}
	const char *url = login.verification_url[0] ? login.verification_url : "https://ya.ru/device";
	const char *scheme = strstr(url, "://");
	if (code_value) {
		lv_label_set_text(code_value, login.user_code);
	}
	if (where_value) {
		lv_label_set_text(where_value, scheme ? scheme + 3 : url);
	}
	if (wait_value) {
		lv_label_set_text(wait_value, "waiting for the code");
	}
	login_elapsed = 0;
	if (!login_timer) {
		login_timer = lv_timer_create(login_tick_cb, 1000, NULL);
	}
	free(a);
}

static void poll_async(void *user) {
	poll_answer_t *a = user;
	login_polling = false;
	// A successful answer counts even if the page stopped waiting meanwhile: the
	// tokens are already written and the account is signed in, and dropping it
	// would leave the screen saying "not signed in" to someone who typed in time.
	if (!login_active && a->state != YANDEX_LOGIN_OK) {
		free(a);
		return;
	}
	switch (a->state) {
	case YANDEX_LOGIN_PENDING:
		break; // nobody has typed it yet: keep polling
	case YANDEX_LOGIN_OK:
		login_stop();
		login_clear();
		rebuild_home();
		gui_notify_popup("signed in");
		break;
	case YANDEX_LOGIN_EXPIRED:
		login_ended("the code has expired");
		break;
	case YANDEX_LOGIN_ERROR:
	default:
		login_ended(a->message[0] ? say(a->message) : "sign in failed");
		break;
	}
	free(a);
}

// ---------------------------------------------------------------------------
// the home page
// ---------------------------------------------------------------------------

// One sentence for every action that cannot work without the keys. It names the
// file, because a card without it is the only way to see this: the file ships
// filled in with the release zip.
#define NO_KEYS_POPUP \
	"no keys: Addons/yandex/settings.ini is missing from the card (it ships filled in with the release zip)"

static void quality_refresh(void) {
	if (!quality_pills[0]) {
		return;
	}
	yandex_quality_t current = yandex_get_quality();
	for (int i = 0; i < YANDEX_QUALITY_COUNT; i++) {
		if (quality_pills[i]) {
			settingsrow_pill_active(quality_pills[i], (int)current == i);
		}
	}
	if (!quality_note) {
		return;
	}
	char format[48];
	yandex_last_format(format, sizeof(format));
	if (current != YANDEX_QUALITY_NORMAL && yandex_needs_plus()) {
		lv_label_set_text(quality_note, "The account has no Plus: lossless comes as a lossy file.");
	} else if (format[0]) {
		lv_label_set_text_fmt(quality_note, "Last file: %s", format);
	} else {
		lv_label_set_text(quality_note, "A ceiling: a track without FLAC comes as AAC or MP3.");
	}
}

static void quality_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	yandex_set_quality((yandex_quality_t)(intptr_t)lv_event_get_user_data(e));
	quality_refresh();
}

// Every label the home shows, filled from the addon's state. Separate from
// rebuild_home() because the code flow writes three of these labels while the
// rows are already up.
static void home_values(void) {
	if (keys_value) {
		lv_label_set_text(keys_value, yandex_configured() ? "present" : "no keys");
	}
	if (account_value) {
		const char *name = yandex_display_name();
		lv_label_set_text(account_value, yandex_logged_in() ? (name && name[0] ? name : "signed in") : "not signed in");
	}
	if (token_value) {
		lv_label_set_text(token_value, token_file_present() ? "found in the addon's folder" : "not in the addon's folder");
	}
	quality_refresh();
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

static void section_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	job_kind_t kind = (job_kind_t)(intptr_t)lv_event_get_user_data(e);
	static const char *const TITLES[] = {
		[JOB_LIKED] = "Liked tracks",
		[JOB_ARTISTS] = "Artists",
		[JOB_ALBUMS] = "Albums",
		[JOB_PLAYLISTS] = "Playlists",
		[JOB_NEW_RELEASES] = "New releases",
		[JOB_CHART] = "Chart",
		[JOB_SAVED] = "Saved on the card",
	};
	const char *title =
		(unsigned)kind < sizeof(TITLES) / sizeof(TITLES[0]) && TITLES[kind] ? TITLES[kind] : "Yandex Music";
	open_list(title, kind, NULL, NULL, 0, false);
}

static void search_row_cb(lv_event_t *e) { open_search_cb(e); }

// My Wave is not a list: it is a station whose first batch starts a queue that
// yandexplay/yandexwave keep going. The answer is a list like any other, so it
// goes through the same worker and the same gui_post().
static void wave_async(void *user) {
	job_result_t *r = user;
	wave_busy = false;
	toast_busy_end();
	if (r->list && yandexlist_count(r->list) > 0 && yandexplay_list(r->list, 0, true)) {
		free_result(r);
		return;
	}
	toast_error(r->error[0] ? say(r->error) : "could not start the station");
	free_result(r);
}

static void play_wave_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (!yandex_configured()) {
		gui_notify_popup(NO_KEYS_POPUP);
		return;
	}
	if (!yandex_logged_in()) {
		gui_notify_popup("not signed in");
		return;
	}
	if (wave_busy) {
		gui_notify_popup("the station is already being asked");
		return;
	}
	wave_busy = true;
	toast_busy("asking the station...");
	// The wave answer is an event: it starts a queue, it does not become the
	// list on screen.
	start_job_worker();
	job_t job = {.kind = JOB_WAVE, .generation = list_generation};
	submit(&job);
}

static void sign_in_cb(lv_event_t *e) {
	(void)e;
	if (!yandex_configured()) {
		gui_notify_popup(NO_KEYS_POPUP);
		return;
	}
	start_one_shot(sign_in_main);
}

static void reload_cb(lv_event_t *e) {
	(void)e;
	// The card may have been written while the player ran: the keys, the token,
	// a hand-edited settings.ini. Nothing here talks to the network.
	yandex_reload();
	rebuild_home();
	gui_notify_popup("read again from the card");
}

static void sign_out_cb(lv_event_t *e) {
	(void)e;
	login_stop();
	login_clear();
	yandexdl_cancel_all();
	yandex_logout();
	yandexplay_signed_out();
	rebuild_home();
	gui_notify_popup("signed out");
}

// Ask for a code: one request, in the worker, answered on the GUI thread.
static void code_sign_in_cb(lv_event_t *e) {
	(void)e;
	if (!yandex_configured()) {
		gui_notify_popup(NO_KEYS_POPUP);
		return;
	}
	if (login_active) {
		gui_notify_popup("a code is already waiting");
		return;
	}
	login_stop();
	memset(&login, 0, sizeof(login));
	login_clear();
	if (wait_value) {
		lv_label_set_text(wait_value, "asking for a code...");
	}
	login_active = true;
	start_one_shot(code_main);
}

static void stop_waiting_cb(lv_event_t *e) {
	(void)e;
	if (!login_active) {
		gui_notify_popup("nothing is waiting");
		return;
	}
	login_stop();
	login_clear();
	gui_notify_popup("stopped");
}

static void open_saved_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	open_list("Saved on the card", JOB_SAVED, NULL, NULL, 0, false);
}

// The home is rebuilt rather than patched: it is a settings page of twenty rows,
// and every value on it can change under it (the account, the token, what is
// saved, the card itself).
static void rebuild_home(void) {
	if (!home_container) {
		return;
	}
	lv_obj_clean(home_container);
	quality_note = NULL;
	account_value = NULL;
	keys_value = NULL;
	token_value = NULL;
	code_value = NULL;
	where_value = NULL;
	wait_value = NULL;
	for (int i = 0; i < YANDEX_QUALITY_COUNT; i++) {
		quality_pills[i] = NULL;
	}

	bool in = yandex_logged_in();

	settingsrow_add(home_container, "Account", &account_value, NULL, NULL);
	lv_obj_set_style_text_font(account_value, &font_ui_20, 0);

	// Signing in is the code, and the code needs no file on the card: the page
	// leads there first. The keys are the one thing that has to be put on the
	// card by hand, so when they are missing the page names the file and the
	// four keys, and says the code cannot be asked for without them. token.txt
	// is an old card's leftover and is mentioned only when it is really there.
	if (!yandex_configured()) {
		// A card without the settings file is the only way here: the file ships
		// filled in with the release zip, so the page names what is missing
		// rather than asking anyone to go and find four values.
		note_label(home_container,
				   "No keys: Addons/yandex/settings.ini is missing from the card. It ships filled in with "
				   "the release zip -- the four values in it are the official client's, as README.txt in "
				   "this folder says (docs/SETUP.md, section 3). No code can be asked for without it.");
	} else if (!in) {
		// Without a session there is nothing to ask the service: the page says
		// what it would need. The sections stay: they are what the page is, and
		// opening one says why it is empty instead of hiding the map.
		note_label(home_container,
				   "Not signed in. Tap \"Sign in with a code\": nothing is typed into a file on the "
				   "card, the code is typed on a phone.");
	}
	if (!in && token_file_present()) {
		note_label(home_container,
				   "A token.txt from an older card is here: \"Sign in from the card\" imports it, but "
				   "such a session never renews -- the code does.");
	}

	settingsrow_add(home_container, "My Wave", NULL, play_wave_cb, NULL);
	settingsrow_add(home_container, "Liked tracks", NULL, section_cb, (void *)(intptr_t)JOB_LIKED);
	settingsrow_add(home_container, "Artists", NULL, section_cb, (void *)(intptr_t)JOB_ARTISTS);
	settingsrow_add(home_container, "Albums", NULL, section_cb, (void *)(intptr_t)JOB_ALBUMS);
	settingsrow_add(home_container, "Playlists", NULL, section_cb, (void *)(intptr_t)JOB_PLAYLISTS);
	settingsrow_add(home_container, "New releases", NULL, section_cb, (void *)(intptr_t)JOB_NEW_RELEASES);
	settingsrow_add(home_container, "Chart", NULL, section_cb, (void *)(intptr_t)JOB_CHART);

	char saved[64];
	snprintf(saved, sizeof(saved), "%d", yandexsaved_count());
	lv_obj_t *value = NULL;
	settingsrow_add(home_container, "Saved on the card", &value, open_saved_cb, NULL);
	lv_label_set_text(value, saved);
	lv_obj_set_style_text_font(value, &font_ui_20, 0);

	settingsrow_add(home_container, "Search", NULL, search_row_cb, NULL);

	// The quality ceiling: a card of pills, as every other choice here.
	lv_obj_t *pills = NULL;
	settingsrow_pills(home_container, "audio_quality", &pills);
	static const char *const QUALITY_NAMES[YANDEX_QUALITY_COUNT] = {"Lossless", "High", "Normal"};
	for (int i = 0; i < YANDEX_QUALITY_COUNT; i++) {
		quality_pills[i] = settingsrow_pill_text(pills, QUALITY_NAMES[i], i, quality_pick_cb);
	}
	quality_note = note_label(home_container, "");

	// The account half of the state screen, kept whole: the keys, the token on
	// the card, the code when one is being waited for, and the four actions that
	// change any of it.
	settingsrow_add(home_container, "Keys", &keys_value, NULL, NULL);
	settingsrow_add(home_container, "Token file", &token_value, NULL, NULL);
	settingsrow_add(home_container, "Code", &code_value, NULL, NULL);
	settingsrow_add(home_container, "Where", &where_value, NULL, NULL);
	settingsrow_add(home_container, "Waiting", &wait_value, NULL, NULL);
	// Those three are state, not a form to fill in here: the address is opened
	// on a phone and the code is typed there, which the page has to say out loud
	// or the rows read as something the player is waiting to be given.
	note_label(home_container,
			   "Open Where on a phone already signed in to this account, type Code there and confirm: "
			   "the addon signs in by itself, and Waiting counts the code's lifetime down.");
	login_clear();

	settingsrow_action(home_container, "Sign in with a code", code_sign_in_cb, NULL);
	settingsrow_action(home_container, "Stop waiting", stop_waiting_cb, NULL);
	settingsrow_action(home_container, "Sign in from the card", sign_in_cb, NULL);
	settingsrow_action(home_container, "Read again from the card", reload_cb, NULL);
	settingsrow_action(home_container, "Sign out", sign_out_cb, NULL);

	home_values();
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	// Never on the way out: this is where a token on the card starts a sign-in,
	// and stop() has just waited for threads like that one.
	if (stopping) {
		return;
	}
	// The card may have come or gone since the page was built.
	listart_set_root(card_root[0] ? card_root : NULL);
	rebuild_home();
}

// ---------------------------------------------------------------------------
// what comes back from the worker, on the GUI thread
// ---------------------------------------------------------------------------

static const char *kind_name(job_kind_t kind) {
	switch (kind) {
	case JOB_LIKED:
		return "liked tracks";
	case JOB_ALBUMS:
		return "liked albums";
	case JOB_ARTISTS:
		return "liked artists";
	case JOB_PLAYLISTS:
		return "playlists";
	case JOB_NEW_RELEASES:
		return "new releases";
	case JOB_CHART:
		return "chart";
	case JOB_PLAYLIST:
		return "playlist";
	case JOB_ALBUM:
		return "album";
	case JOB_ARTIST:
		return "artist";
	case JOB_ARTIST_TRACKS:
		return "artist tracks";
	case JOB_SEARCH:
		return "search";
	case JOB_SAVED:
		return "saved on the card";
	case JOB_SAVED_ALBUM:
		return "saved album";
	default:
		return "list";
	}
}

static void job_done_async(void *user) {
	job_result_t *r = user;

	switch (r->kind) {
	case JOB_WAVE:
		wave_async(r);
		return;
	case JOB_LIKE_ALBUM:
	case JOB_LIKE_ARTIST:
		if (r->error[0]) {
			toast_error(say(r->error));
		} else {
			toast_success(r->kind == JOB_LIKE_ALBUM ? "Album updated." : "Artist updated.");
		}
		free_result(r);
		return;
	case JOB_LIKES:
		// Only the log wants to know: a failure leaves the menus saying "Like"
		// for a track that may already be one, which is what the missing mirror
		// means, and the next read of it fixes that.
		if (r->error[0]) {
			fprintf(stderr, "yandex: the liked tracks were not read (%s)\n", r->error);
		}
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
		} else {
			playing_update();
			// Another page of details arrived. The list is redrawn from `shown`
			// in place, so the reader's place is kept by construction.
			list_render();
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
	if (list_spin) {
		lv_obj_set_hidden(list_spin, true);
	}
	free_result(shown);
	shown = r;
	playing_update();
	list_render();
	printf("yandex: %s: %d entries%s%s\n", kind_name(r->kind), view_count(), r->error[0] ? ", " : "",
		   r->error[0] ? r->error : "");

	if (autoplay_pending) {
		autoplay_pending = false;
		if (view_count() > 0) {
			play_all();
		}
	}
	// A session that just ended (the token expired) shows on the home too.
	if (!yandex_logged_in()) {
		rebuild_home();
	}
	if (!yandex_liked_known()) {
		run(JOB_LIKES, NULL, NULL, 0);
	}
}

// The downloads talk to the interface through the worker: the listener runs
// there, and gui_post() brings each event over.
typedef struct {
	yandexdl_event_t event;
	int count;
	char text[400];
} save_note_t;

static void save_note_async(void *user) {
	save_note_t *note = user;
	char line[480];
	switch (note->event) {
	case YANDEXDL_QUEUED:
		snprintf(line, sizeof(line), "Saving: %s", note->text);
		toast_success(line);
		break;
	case YANDEXDL_SAVED:
		refresh_after_change();
		break;
	case YANDEXDL_EPISODE_SAVED:
	case YANDEXDL_BOOK_SAVED:
		refresh_after_change();
		toast_success("Saved to the card.");
		break;
	case YANDEXDL_FAILED:
		toast_error(note->text);
		break;
	case YANDEXDL_FINISHED:
		if (note->count > 0) {
			snprintf(line, sizeof(line), "%d saved to %s", note->count, note->text);
			toast_success(line);
		}
		refresh_after_change();
		break;
	case YANDEXDL_STOPPED:
		toast_plain(note->text);
		break;
	}
	free(note);
}

static void save_listener(yandexdl_event_t event, int count, const char *text) {
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
// the page
// ---------------------------------------------------------------------------

static void play_tick_cb(lv_timer_t *timer) {
	(void)timer;
	yandexplay_watch();
}

// The service half of the library: the album page and the mark over the
// artwork. The mark would be an lv_image_dsc_t and this player's icon set has no
// Yandex badge, so the artwork keeps none (as Zvuk's does).
static void *open_album_for_player(const char *album_id, const char *title) {
	if (!album_id || !album_id[0]) {
		return NULL;
	}
	open_list(title && title[0] ? title : "Album", JOB_ALBUM, album_id, NULL, 0, false);
	return list_screen;
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	// The page is off screen: stop polling, or it is one request every few
	// seconds for a code nobody is looking at. A poll already in flight finishes
	// and is dropped by poll_async().
	if (login_active) {
		login_stop();
	}
	login_clear();
}

void yandexstate_init(gui_config_t *cfg, const char *sd_root, const char *dir, const char *id) {
	// stop() left the two flags where a fresh page wants them when it could
	// wait for the workers; when it could not, the library is still mapped and
	// the worker that outlived it must be the one that ends before another
	// starts, so nothing is reset here.
	page_cfg = cfg;
	own_id = id;
	snprintf(card_root, sizeof(card_root), "%s", sd_root ? sd_root : "");
	snprintf(addon_dir, sizeof(addon_dir), "%s", dir ? dir : "");
	listart_set_root(card_root[0] ? card_root : NULL);

	yandexstate_screen = lv_obj_create(NULL);
	// Registered as it is created (addon.h): the shell switches off and deletes
	// every screen an addon owns when the library is unloaded, and it cannot
	// know about this one otherwise -- init()'s return value is the only screen
	// it would have.
	sonix_addon_own_screen(own_id, yandexstate_screen);
	home_container = settingsrow_page(yandexstate_screen, cfg, "Yandex Music");

	build_list_page(cfg);
	build_search_page(cfg);

	rebuild_home();

	lv_obj_add_event_cb(yandexstate_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(yandexstate_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(yandexstate_screen);
	player_sheet_attach_drag(yandexstate_screen, true);
	player_sheet_attach_drag(home_container, true);
	// The pills and the note are hand-coloured: a theme or accent change has to
	// repaint them, and the hook is code in this library that stop() must take
	// back.
	theme_register_refresh(quality_refresh);

	// The downloads report here, so the list and the home stay true while
	// something is being saved, and playback's events reach the spinner and the
	// rows.
	yandexdl_set_listener(save_listener);
	yandexplay_init(playback_event);

	// The playback watch: fetching ahead, the cache's ceiling, My Wave's next
	// batch. One second, for as long as the library is loaded -- whether the
	// page is up or not: the music outlives the screen that started it, and only
	// stop() (the card leaving, or the addon being replaced) ends it.
	play_timer = lv_timer_create(play_tick_cb, 1000, NULL);

	// The service table is this library's and the page is the only part of the
	// player that can fill its two interface fields in.
	yandex_service_attach_gui(NULL, open_album_for_player);
}

// ---------------------------------------------------------------------------
// stopping
//
// Runs first in the addon's stop(), before the service modules: the timers are
// what would otherwise keep calling into them (and into LVGL) after the library
// has been unmapped, and this page's workers are the ones that read the state
// those modules are about to free.
// ---------------------------------------------------------------------------

bool yandexstate_stop(void) {
	// Nothing new starts from here: the waits below take seconds, and a handler
	// that ran in between would start a worker again.
	pthread_mutex_lock(&job_lock);
	stopping = true;
	yandexlist_free(page_pending.list);
	page_pending = (job_t){0};
	pending = (job_t){0};
	events_count = 0;
	pthread_cond_broadcast(&job_cond);
	pthread_mutex_unlock(&job_lock);

	// The clocks. A deleted timer cannot fire again, and a sign-in nobody is
	// looking at must not poll a code on the way out.
	login_stop();
	if (play_timer) {
		lv_timer_delete(play_timer);
		play_timer = NULL;
	}
	theme_unregister_refresh(quality_refresh);

	// No more reports from the save queue, and the covers of the rows on screen
	// go back to listart before the rows themselves are deleted -- by the shell,
	// after this returns. listrow_stop() takes the poll timer with it: it is not
	// a child of any widget and its callback is code in this library.
	yandexdl_set_listener(NULL);
	listrow_reset((void *)&yandex_art_owner);
	listrow_stop();

	// A row menu left open is on the player's top layer, not on this screen: it
	// would keep the tap that runs do_like()/do_save() -- code here -- reachable
	// over whatever page came next. The same for the confirm card the delete
	// path opens and a "waiting" toast, which is never taken down by itself.
	popover_close();
	confirm_close();
	if (toast_busy_showing()) {
		toast_busy_end();
	}

	// The page's workers -- a list, a page fetch, a like, a token, a code, a poll
	// -- are one request each and have no flag of their own to be asked to stop:
	// they are waited for instead. A poll or a page in flight is a request the
	// player is already paying for (http.h cannot be woken out of http_request),
	// so the wait is bounded; a false here is a thread still inside the library,
	// and addon_unload() keeps the mapping when it hears one.
	bool gone = page_wait("the page");

	// The result array is the worker's own scratch space, so it goes only when
	// the thread that writes it is gone; `page_pending` above is what a job of
	// the list's next page held.
	pthread_mutex_lock(&job_lock);
	if (gone) {
		worker_started = false;
		stopping = false; // a second init() starts a fresh worker
		free(result);
		result = NULL;
		yandexlist_free(result_list);
		result_list = NULL;
	}
	list_generation++;
	pthread_mutex_unlock(&job_lock);

	// The list on screen: its ids are this page's, and the player's playlist is
	// not the same thing and does not point here.
	free_result(shown);
	shown = NULL;

	// Everything below pointed into the screens the shell deletes once this
	// returns, and a second init() in the same process would write through it.
	home_container = NULL;
	list_view = NULL;
	list_body = NULL;
	list_title = NULL;
	list_empty = NULL;
	list_spin = NULL;
	search_field = NULL;
	search_keyboard = NULL;
	quality_note = NULL;
	account_value = NULL;
	keys_value = NULL;
	token_value = NULL;
	code_value = NULL;
	where_value = NULL;
	wait_value = NULL;
	for (int i = 0; i < YANDEX_QUALITY_COUNT; i++) {
		quality_pills[i] = NULL;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		memset(&pool[i], 0, sizeof(pool[i]));
		pool[i].index = -1;
	}

	// And the state, so that a second init() starts where the first one did
	// rather than on what the last one happened to show.
	page_cfg = NULL;
	own_id = NULL;
	card_root[0] = '\0';
	addon_dir[0] = '\0';
	list_kind = JOB_NONE;
	page_asked = -1;
	page_failed = -1;
	page_failed_at = 0;
	playing_id[0] = '\0';
	autoplay_pending = false;
	wave_busy = false;
	list_history_len = 0;
	list_has_current = false;
	memset(&list_current, 0, sizeof(list_current));
	memset(&menu_row, 0, sizeof(menu_row));
	memset(&delete_row, 0, sizeof(delete_row));
	menu_index = 0;
	result_count = 0;
	result_error[0] = '\0';
	result_title[0] = '\0';
	memset(&login, 0, sizeof(login));
	login_active = false;
	login_polling = false;
	login_elapsed = 0;
	yandexstate_screen = NULL;
	list_screen = NULL;
	search_screen = NULL;

	// The service table's two GUI halves point at this page. The player
	// unregisters the table itself; emptying them here is for the unload that
	// fails and leaves the library mapped.
	yandex_service_attach_gui(NULL, NULL);
	return gone;
}
