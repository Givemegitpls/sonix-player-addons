#include "tidalservice.h"

#include "src/addon/addon_service.h"

#include <stdio.h>
#include <stdlib.h>

#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/tidalsync.h"

// See tidalservice.h. Every function here is a one-liner over the existing
// module: the shape is the one rekkod's provider.h worked out (docs/ADDONS.md,
// "A service rather than an app"), and none of them adds behaviour.

static void set_root(const char *sd_root) { tidalcache_set_root(sd_root); }
static const char *cache_dir(void) { return tidalcache_dir(); }
static bool owns_path(const char *path) { return tidalcache_owns(path); }
static int track_id(const char *path) { return (int)tidalcache_track_id(path); }
static bool downloading(void) { return tidalcache_downloading_id() > 0; }
static bool network_wanted(void) { return tidalcache_network_wanted(); }
static void clear_on_exit(void) { tidalcache_clear_on_exit(); }

// The account half speaks in paths, like the service table: the page and the
// control centre have the file, and the track id is Tidal's own business.
static bool fav_get(const char *path) {
	long id = tidalcache_track_id(path);
	return id > 0 && tidalsync_is_favorite(id);
}

static void fav_refresh(void (*done)(void)) { tidalsync_favorites_refresh(done); }

static void fav_toggle(const char *path, bool want, sonix_addon_service_done_cb done, void *user) {
	long id = tidalcache_track_id(path);
	if (id <= 0) {
		if (done) {
			done(false, "not one of this service's tracks", user);
		}
		return;
	}
	tidalsync_favorite_toggle(id, want, (tidalsync_done_cb)done, user);
}

static bool album_id(const char *path, char *out, size_t size) { return tidalcache_album_id(path, out, size); }

static bool playlists_known(void) { return tidalsync_playlists_known(); }
static void playlists_refresh(void (*done)(void)) { tidalsync_playlists_refresh(done); }

static int playlists(sonix_addon_playlist_t *out, int max) {
	tidal_playlist_t got[TIDALSYNC_PLAYLISTS_MAX];
	int total = tidalsync_playlists(got, TIDALSYNC_PLAYLISTS_MAX);
	if (total <= 0) {
		return total;
	}
	// Tidal's playlist ids are UUIDs, which the table took as text already.
	int count = total < max ? total : max;
	for (int i = 0; i < count; i++) {
		snprintf(out[i].id, sizeof(out[i].id), "%s", got[i].id);
		snprintf(out[i].name, sizeof(out[i].name), "%s", got[i].name);
		out[i].track_count = got[i].track_count;
	}
	return count;
}

static void playlist_add(const char *playlist_id, const char *path, sonix_addon_service_done_cb done, void *user) {
	// A UUID, unlike Qobuz's number -- which is why the table's id is text.
	long id = tidalcache_track_id(path);
	if (!playlist_id || !playlist_id[0] || id <= 0) {
		if (done) {
			done(false, "not one of this service's tracks", user);
		}
		return;
	}
	tidalsync_playlist_add(playlist_id, id, (tidalsync_done_cb)done, user);
}

static void playlist_create_with(const char *name, const char *path, sonix_addon_service_done_cb done, void *user) {
	long id = tidalcache_track_id(path);
	if (id <= 0) {
		if (done) {
			done(false, "not one of this service's tracks", user);
		}
		return;
	}
	tidalsync_playlist_create_with(name, id, (tidalsync_done_cb)done, user);
}

// Not const: the badge and the album page are filled in by the page
// (tidal_service_attach_gui), long after this table is registered.
static sonix_addon_service_t tidal = {
	.id = "tidal",
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
	.playlists_known = playlists_known,
	.playlists_refresh = playlists_refresh,
	.playlists = playlists,
	.playlist_add = playlist_add,
	.playlist_create_with = playlist_create_with,
};

const sonix_addon_service_t *tidal_service_table(void) { return &tidal; }

void tidal_service_attach_gui(const void *badge, void *(*open_album)(const char *album_id, const char *title)) {
	tidal.badge = badge;
	tidal.open_album = open_album;
}
