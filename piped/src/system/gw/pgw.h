#ifndef PGW_H
#define PGW_H

#include <stdbool.h>
#include <stddef.h>

// Piped: YouTube, read through a public Piped instance.
//
// Why an instance and not YouTube itself: the player's own network cannot
// reach www.youtube.com, youtubei.googleapis.com or any googlevideo host --
// TCP connects, then nothing (measured 2026-11-05). A Piped instance answers
// from a host that is reachable and hands back a media URL on its own
// /videoplayback proxy, which is reachable too.
//
// What the instances actually serve today, measured on the two live ones
// (api.piped.private.coffee, pipedapi.ducks.party):
//
//   /search?q=&filter=music_songs     works: title, id, duration, uploader,
//                                     thumbnail, 20 items
//   /suggestions?query=               works
//   /config                           works; imageProxyUrl is where covers go
//   /streams/{id}                     works: title, duration, uploader,
//                                     thumbnail, relatedStreams (20), and a
//                                     proxied media URL
//   /playlists/{id}, /channel/{id}    answer 200 with nextpage null and an
//                                     empty relatedStreams -- the extractor
//                                     behind them is broken. Not used.
//
// Only muxed MP4 (itag 18) is offered: audioStreams is empty on both, and the
// player has no WebM demuxer, so there is nothing else to choose. itag 18
// carries an AAC track and its moov box is ahead of mdat (checked), which is
// what makes playback on the growing file possible.
//
// Everything here blocks on the network and must be called from a worker
// thread, never from the UI thread (docs/ADDONS.md, "Threads").

#define PGW_ID_MAX 32
#define PGW_TITLE_MAX 200
#define PGW_NAME_MAX 128
#define PGW_URL_MAX 1200
#define PGW_MIME_MAX 32
#define PGW_ITEMS_MAX 24
#define PGW_INSTANCES_MAX 8
#define PGW_INSTANCE_MAX 160

typedef enum {
	PGW_OK = 0,
	PGW_ERR_NET,	  // no instance answered: DNS, TLS, timeout, 5xx
	PGW_ERR_INSTANCE, // answered, but not with what Piped answers (proxy page, dead backend)
	PGW_ERR_NOT_FOUND, // the instance says the video does not exist or is unavailable
	PGW_ERR_FORMAT,	  // answered, but no playable format in it
} pgw_err_t;

// One row of a search result or of "related".
typedef struct {
	char id[PGW_ID_MAX]; // videoId, the key every other call takes
	char title[PGW_TITLE_MAX];
	char uploader[PGW_NAME_MAX];
	char thumbnail[PGW_URL_MAX]; // absolute, already through the instance's image proxy
	int duration;				 // seconds, 0 when unknown
	bool live;
} pgw_item_t;

// One playable video: the row above plus the media URL.
typedef struct {
	char id[PGW_ID_MAX];
	char title[PGW_TITLE_MAX];
	char uploader[PGW_NAME_MAX];
	char thumbnail[PGW_URL_MAX];
	char url[PGW_URL_MAX]; // the instance's /videoplayback URL, "" when there is none
	char mime[PGW_MIME_MAX];
	int itag;
	int duration;
	int bitrate_kbps; // 0 when the instance does not say
	bool live;
} pgw_track_t;

// Reads the addon's settings, the built-in instance list and the last known
// instance. `dir` is sonix_addon_host_t::dir, `sd_root` the card root; both
// NULL or "" is allowed and means "nothing is stored" (the built-in list still
// works, settings are not written).
void pgw_init(const char *addon_dir, const char *sd_root);

// Drops the last response and every pointer into it, for the addon's stop().
// Never frees the settings: init() decides what a second run starts from.
void pgw_forget(void);

// The instance in use, "" when none has been probed successfully yet.
const char *pgw_instance(void);

// Pins the instance to `url` (scheme optional, trailing slash ignored) and
// saves it. An empty string means "any of the built-in ones", which is what a
// fresh install is.
bool pgw_set_instance(const char *url);

// The built-in list, in the order they are tried.
int pgw_instance_count(void);
const char *pgw_instance_at(int index);

// Probes instances until one answers a known request, then remembers it.
// Called from the worker that starts a search, and from the settings page's
// "test" row. `timeout_secs` bounds every request it makes.
pgw_err_t pgw_health(int timeout_secs);

// Search, `filter` is Piped's own ("music_songs", "videos", "" for everything).
// Writes at most `max` items into `out`, `*count` gets how many.
pgw_err_t pgw_search(const char *query, const char *filter, pgw_item_t *out, int max, int *count);

// One playable video, with the media URL and the format chosen.
pgw_err_t pgw_track(const char *id, pgw_track_t *out);

// "Related" as the instance reports it for a video: what the queue offers next.
pgw_err_t pgw_related(const char *id, pgw_item_t *out, int max, int *count);

// A sentence for the page, never NULL.
const char *pgw_error_text(pgw_err_t err);

#endif /* PGW_H */
