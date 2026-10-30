#include "yandexservice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexcache.h"
#include "src/system/streaming/yandexdl.h"
#include "src/system/streaming/yandexsaved.h"
#include "src/system/streaming/yandexsync.h"

// See yandexservice.h. Every function here is a one-liner over an existing
// module: the shape is the one the fork's provider.h worked out (its
// stream_provider_t was filled in at the end of yandexsync.c), and none of them
// adds behaviour. The cache hooks are yandexcache/yandexdl's own; the account
// ones go through yandexsync, whose worker does the write and whose mirror
// answers the star at once.

static void set_root(const char *sd_root) {
	yandexcache_set_root(sd_root);
	yandexdl_set_root(sd_root);
	yandexsaved_set_root(sd_root);
}

static const char *cache_dir(void) { return yandexcache_dir(); }

// What is Yandex's: the cache it downloads into, and the files it saved into
// the library while this queue claims them (yandexsaved_claim). A saved file
// that the library opened on its own is the library's, not Yandex's.
static bool owns_path(const char *path) { return yandexcache_owns(path) || yandexsaved_claimed(path); }

// The track id behind a path: the cache's file name, or the index's entry.
static void track_ids(const char *path, char *id, size_t id_size, char *album, size_t album_size) {
	if (album && album_size) {
		album[0] = '\0';
	}
	yandexcache_track_id(path, id, id_size);
	if (id[0]) {
		if (album && album_size && !yandexcache_album_id(path, album, album_size)) {
			album[0] = '\0';
		}
		return;
	}
	yandexsaved_track_for_path(path, id, id_size, album, album ? album_size : 0);
}

// "Is this an actual track of the service" is track_id() != 0 below: the
// interface this player has asks it that way (the fork had a separate is_track
// hook, and a track is what has an id).
//
// The interface numbers it in an int (Qobuz's ids are small too). A Yandex
// catalogue id is a decimal number of at most nine digits, which fits, and the
// caller only compares it with itself; a user's upload has a non-numeric id and
// answers 0, which is the same answer the interface gives for "not a track".
static int track_id(const char *path) {
	char id[YANDEX_ID_MAX], album[YANDEX_ID_MAX];
	track_ids(path, id, sizeof(id), album, sizeof(album));
	if (!id[0] || strspn(id, "0123456789") != strlen(id)) {
		return 0;
	}
	return (int)strtol(id, NULL, 10);
}

static bool downloading(void) { return yandexcache_busy() || yandexdl_busy(); }

static bool network_wanted(void) { return yandexcache_network_wanted() || yandexdl_busy(); }

// The cache is transient, swept on the way out; what was saved into the library
// is the user's and is never touched.
static void clear_on_exit(void) {
	yandexdl_cancel_all();
	yandexcache_clear_on_exit();
}

static bool fav_get(const char *path) {
	char id[YANDEX_ID_MAX];
	track_ids(path, id, sizeof(id), NULL, 0);
	return id[0] && yandex_is_liked(id);
}

static void fav_refresh(void (*done)(void)) { yandexsync_likes_refresh(done); }

static void fav_toggle(const char *path, bool want, sonix_addon_service_done_cb done, void *user) {
	char id[YANDEX_ID_MAX], album[YANDEX_ID_MAX];
	track_ids(path, id, sizeof(id), album, sizeof(album));
	if (!id[0]) {
		if (done) {
			done(false, "not one of this service's tracks", user);
		}
		return;
	}
	yandexsync_like(id, album, want, (yandexsync_done_cb)done, user);
}

static bool album_id(const char *path, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	char id[YANDEX_ID_MAX];
	track_ids(path, id, sizeof(id), out, size);
	return out[0] != '\0';
}

// Not const: the badge and the album page are filled in by the page
// (yandex_service_attach_gui), long after this table is registered.
static sonix_addon_service_t yandex = {
	.id = "yandex",
	.set_root = set_root,
	.cache_dir = cache_dir,
	.owns_path = owns_path,
	.track_id = track_id,
	.downloading = downloading,
	.network_wanted = network_wanted,
	.clear_on_exit = clear_on_exit,
	// A factory reset takes the same: everything this service keeps on the card
	// is its cache, and the files it saved into the library are the user's.
	.wipe = NULL,
	.fav_get = fav_get,
	.fav_refresh = fav_refresh,
	.fav_toggle = fav_toggle,
	.album_id = album_id,
	// Yandex's playlists are not offered to "Add to playlist": a cached track
	// goes to the card's playlists, as before, and the NULL hooks say so.
	.playlists_known = NULL,
};

const sonix_addon_service_t *yandex_service_table(void) { return &yandex; }

void yandex_service_attach_gui(const void *badge, void *(*open_album)(const char *album_id, const char *title)) {
	yandex.badge = badge;
	yandex.open_album = open_album;
}
