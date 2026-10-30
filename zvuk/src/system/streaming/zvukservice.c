#include "zvukservice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/streaming/zvukcache.h"
#include "src/system/streaming/zvukdl.h"
#include "src/system/streaming/zvukplay.h"
#include "src/system/streaming/zvuksync.h"

// See zvukservice.h. Every function here is a one-liner over an existing
// module: the shape is the one the fork's provider.h worked out, and none of
// them adds behaviour. The cache hooks are zvukcache/zvukdl's own; the account
// ones go through zvuksync, whose worker does the write and whose mirror
// answers the star at once.

static void set_root(const char *sd_root) {
	zvukcache_set_root(sd_root);
	zvukdl_set_root(sd_root);
}

static const char *cache_dir(void) { return zvukcache_dir(); }

// What is Zvuk's: the cache it downloads into, and the files it saved into the
// library while this session claims them (zvukdl_claim). A saved file that the
// library opened on its own is the library's, not Zvuk's.
static bool owns_path(const char *path) { return zvukcache_owns(path) || zvukdl_claimed(path); }

// "Is this an actual track of the service" is track_id() != 0 below: the
// interface this player has asks it that way (the fork had a separate is_track
// hook, and a track is what has an id).

// The service's own id for a cache file, 0 for a sidecar or anything else. The
// interface numbers it in an int (Qobuz's ids are small too); a Zvuk id is a
// decimal number of at most nine digits, and the caller only compares it with
// itself.
static int track_id(const char *path) {
	char id[ZVUK_ID_MAX];
	id[0] = '\0';
	zvukcache_track_id(path, id, sizeof(id));
	if (!id[0]) {
		char album[ZVUK_ID_MAX];
		if (!zvukdl_track_for_path(path, id, sizeof(id), album, sizeof(album))) {
			return 0;
		}
	}
	return id[0] ? (int)strtol(id, NULL, 10) : 0;
}

static bool downloading(void) { return zvukcache_busy() || zvukdl_busy(); }

static bool network_wanted(void) { return zvukcache_network_wanted() || zvukdl_busy(); }

// The cache is transient, swept on the way out; what was saved into the library
// is the user's and is never touched.
static void clear_on_exit(void) {
	zvukdl_cancel_all();
	zvukcache_clear_on_exit();
}

// The track's id from its path, whichever of the two places the file is in.
static bool id_for_path(const char *path, char *id, size_t size, char *album, size_t album_size) {
	zvukcache_track_id(path, id, size);
	if (id[0]) {
		if (album && album_size && !zvukcache_album_id(path, album, album_size)) {
			album[0] = '\0';
		}
		return true;
	}
	if (!zvukdl_track_for_path(path, id, size, album, album_size)) {
		id[0] = '\0';
		return false;
	}
	return true;
}

static bool fav_get(const char *path) {
	char id[ZVUK_ID_MAX];
	if (!id_for_path(path, id, sizeof(id), NULL, 0)) {
		return false;
	}
	return zvuk_is_liked(ZVUK_ITEM_TRACK, id);
}

static void fav_refresh(void (*done)(void)) { zvuksync_likes_refresh(done); }

static void fav_toggle(const char *path, bool want, sonix_addon_service_done_cb done, void *user) {
	char id[ZVUK_ID_MAX];
	if (!id_for_path(path, id, sizeof(id), NULL, 0)) {
		if (done) {
			done(false, "not one of this service's tracks", user);
		}
		return;
	}
	zvuksync_like(ZVUK_ITEM_TRACK, id, want, (zvuksync_done_cb)done, user);
}

static bool album_id(const char *path, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	char id[ZVUK_ID_MAX];
	if (!id_for_path(path, id, sizeof(id), out, size)) {
		return false;
	}
	return out[0] != '\0';
}

// Not const: the badge and the album page are filled in by the page
// (zvuk_service_attach_gui), long after this table is registered.
static sonix_addon_service_t zvuk = {
	.id = "zvuk",
	.set_root = set_root,
	.cache_dir = cache_dir,
	.owns_path = owns_path,
	.track_id = track_id,
	.downloading = downloading,
	.network_wanted = network_wanted,
	.clear_on_exit = clear_on_exit,
	.fav_get = fav_get,
	.fav_refresh = fav_refresh,
	.fav_toggle = fav_toggle,
	.album_id = album_id,
	// Zvuk's account has no playlists an outside client may write to: the
	// "Add to playlist" picker offers nothing for it, which the NULL hooks say.
	.playlists_known = NULL,
};

const sonix_addon_service_t *zvuk_service_table(void) { return &zvuk; }

void zvuk_service_attach_gui(const void *badge, void *(*open_album)(const char *album_id, const char *title)) {
	zvuk.badge = badge;
	zvuk.open_album = open_album;
}
