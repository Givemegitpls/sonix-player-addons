#ifndef QOBUZ_H
#define QOBUZ_H

#include <stdbool.h>
#include <stddef.h>

// Qobuz.
//
// Reconstructed from the original binary (`../app/qobuz/lg_qobuz_api.c` in its
// asserts), which talks to the Qobuz API directly: no HiBy server in between,
// nothing to reimplement on the network side beyond the requests.
//
// Two deliberate differences from the original, both improvements:
//
//   * the original uses `http://www.qobuz.com/api.json/0.2` -- cleartext, with
//     the password in a header. This starts from https, with the certificate
//     verification http.c actually performs (the original disables it:
//     CURLOPT_SSL_VERIFYPEER 0 everywhere, and its rootfs has no CA bundle at
//     all);
//   * the original keeps username and password in cleartext in
//     /data/qobuz_info and logs in again at every boot. Here there is no
//     password at all: the account's user_auth_token is taken from a file in
//     the addon's own folder on the card (below), and only that token is kept.
//
// The application credentials (app_id, app_secret) are not in here: they are
// the addon's own, in <card>/Addons/qobuz/settings.ini, see qobuzconf.h.
//
// Every function that talks to the network blocks. Call them from a worker
// thread, never from the GUI thread -- the same rule as http.h and radio.c.

// ---------------------------------------------------------------------------
// Quality
//
// The numbers are Qobuz's own, the ones the original uses.
// ---------------------------------------------------------------------------

typedef enum {
	QOBUZ_FORMAT_MP3 = 5,	  // MP3 320 kbps
	QOBUZ_FORMAT_CD = 6,	  // FLAC 16 bit / 44.1 kHz
	QOBUZ_FORMAT_HIRES96 = 7, // FLAC up to 24/96
	QOBUZ_FORMAT_HIRES192 = 27, // FLAC up to 24/192
} qobuz_format_t;

// What is requested from Qobuz. Stored in the configuration; Qobuz serves the
// best the subscription allows up to what was asked for, so lowering it saves
// card space and waiting rather than giving up quality one was entitled to.
int qobuz_get_format(void);
void qobuz_set_format(int format_id);

// The file type expected at the currently selected quality. It gives the name a
// track's file will have before it has been requested, which is what allows
// queueing a whole album and downloading it while it plays, instead of queueing
// only what has already arrived.
//
// MP3 only when MP3 was requested; for everything else Qobuz sends FLAC (at a
// resolution that may be lower than requested, but always FLAC).
const char *qobuz_expected_mime(void);

// ---------------------------------------------------------------------------
// What the lists show
// ---------------------------------------------------------------------------

#define QOBUZ_TITLE_MAX 200
#define QOBUZ_NAME_MAX 128
#define QOBUZ_URL_MAX 512

typedef struct {
	long id;
	char title[QOBUZ_TITLE_MAX];
	char artist[QOBUZ_NAME_MAX];
	char album[QOBUZ_TITLE_MAX];
	char album_id[40]; // to reopen the album the track comes from
	char cover[QOBUZ_URL_MAX];
	int duration;			   // seconds
	int track_number;
	bool hires;
	bool streamable; // false = present but not playable on this subscription
	int bit_depth;
	int sample_rate; // Hz
} qobuz_track_t;

typedef struct {
	long id; // Qobuz album ids are alphanumeric, so id_text is the real one
	char id_text[40];
	char title[QOBUZ_TITLE_MAX];
	char artist[QOBUZ_NAME_MAX];
	char cover[QOBUZ_URL_MAX];
	char released[16];
	int track_count;
	bool hires;
} qobuz_album_t;

typedef struct {
	long id;
	char name[QOBUZ_NAME_MAX];
	char image[QOBUZ_URL_MAX];
	int album_count;
} qobuz_artist_t;

typedef struct {
	long id;
	char name[QOBUZ_TITLE_MAX];
	char owner[QOBUZ_NAME_MAX];
	char image[QOBUZ_URL_MAX];
	int track_count;
} qobuz_playlist_t;

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

// The account half of the service, as a token file in the addon's own folder on
// the card: one line, the account's `user_auth_token` as the Qobuz web player
// has it.
//
// There is no username/password form, and none is to be added back: with the
// application pair this addon derives from the web player, Qobuz's /user/login
// answers "invalid credentials" for accounts that are perfectly valid, so a
// form with two fields could only be a button that does not work. Every call
// after a login carries the token in X-User-Auth-Token anyway, so the token is
// the whole of what the account half needs.
#define QOBUZ_TOKEN_FILE "token.txt"
#define QOBUZ_TOKEN_REL "Addons/qobuz/token.txt"

// What qobuz_sign_in_from_card() answered.
typedef enum {
	QOBUZ_CARD_SIGNED_IN = 0, // the token works and is the session now
	QOBUZ_CARD_NO_TOKEN,	  // no file, or nothing that reads as a token in it
	QOBUZ_CARD_NO_KEYS,		  // no application pair: a token alone cannot be checked
	QOBUZ_CARD_REFUSED,		  // Qobuz answered, and not with a session
} qobuz_card_result_t;

// Blocks. Reads <card>/Addons/qobuz/token.txt, asks Qobuz who the token belongs
// to (one /user/get, the only call whose business is the account itself) and,
// when the server accepts it, makes it the session and writes session.ini.
//
// `keep_current` is "Read again": a token or a server answer that is no good
// leaves the session as it was. With it false -- "Sign in from the card", and
// the check init() starts -- a failure signs out, so no session is kept that the
// server rejected. Qobuz's own sentence, when it gave one, is in
// qobuz_last_error(). The token is never logged and never returned.
qobuz_card_result_t qobuz_sign_in_from_card(bool keep_current);

// True while the token in memory came from the card in this run and Qobuz has
// not accepted it yet: the page asks on a worker, never on the GUI thread.
bool qobuz_session_unchecked(void);

// True when the card refused the write of the session (a read-only card): what
// is signed in works for this run and is gone at the next start.
bool qobuz_session_not_kept(void);

// Reads the application credentials and the token. Once, from the addon's
// init(), after qobuzconf_init(). A token file that is there becomes the
// session immediately, unchecked (see above) -- unless the last thing written
// was a sign-out, which a restart must not undo. It is also where the commented
// template for a fresh card is written, once.
void qobuz_init(void);

// Reads the credentials and the token again, after the card may have been
// written (qobuzconf_reload()).
void qobuz_reload_session(void);

// Drops what qobuz_init() keeps in memory, for the addon's stop(): see
// addon.h, "Unloading". The next init() reads the card again and must not find
// anything of the previous run still set.
void qobuz_forget(void);

// False when the keys file is missing: the service is not broken, it is off,
// and the menu entry must say so instead of trying and failing.
bool qobuz_configured(void);

bool qobuz_logged_in(void);
const char *qobuz_display_name(void); // empty when not logged in

// Forgets the token, here and in the configuration, and records the sign-out so
// that a restart does not read the token file straight back in.
void qobuz_logout(void);

// Why the last call failed, already in a form fit to display. Empty on success.
// Per thread, like http_last_error().
const char *qobuz_last_error(void);

// ---------------------------------------------------------------------------
// Requests. All return how many items they wrote, or -1 on error
// (`qobuz_last_error()` says what happened). All block.
// ---------------------------------------------------------------------------

int qobuz_search_tracks(const char *query, int offset, qobuz_track_t *out, int max);
int qobuz_search_albums(const char *query, int offset, qobuz_album_t *out, int max);
int qobuz_search_artists(const char *query, int offset, qobuz_artist_t *out, int max);

// The contents of a single item are paged too: a three-hundred-track playlist
// does not fit in one response, and neither does a prolific artist. `offset`
// says which entry to start from, as for searches.
int qobuz_album_tracks(const char *album_id, int offset, qobuz_track_t *out, int max);
int qobuz_artist_albums(long artist_id, int offset, qobuz_album_t *out, int max);
int qobuz_playlist_tracks(long playlist_id, int offset, qobuz_track_t *out, int max);

int qobuz_favorite_tracks(int offset, qobuz_track_t *out, int max);
int qobuz_favorite_albums(int offset, qobuz_album_t *out, int max);
int qobuz_favorite_artists(int offset, qobuz_artist_t *out, int max);
int qobuz_user_playlists(int offset, qobuz_playlist_t *out, int max);

// The collections Qobuz curates: `type` is one of its own ("new-releases",
// "editor-picks", "best-sellers", "press-awards", "most-streamed").
int qobuz_featured_albums(const char *type, int offset, qobuz_album_t *out, int max);

// The signed URL of a track's audio file. `mime` may be NULL.
//
// The URL is valid for a few minutes and must not be stored: request it every
// time the track is played.
bool qobuz_track_url(long track_id, char *url, size_t url_size, char *mime, size_t mime_size, int *format_out);

// ---------------------------------------------------------------------------
// Writing to the account
//
// For a Qobuz track, the player's favourite star and "Add to playlist" must end
// up here and not in the local database: the cache file being played is
// transient, and a favourite pointing at a path bound to disappear is not a
// favourite.
// ---------------------------------------------------------------------------

bool qobuz_favorite_add_track(long track_id);
bool qobuz_favorite_remove_track(long track_id);

// The ids of the favourite tracks, to know how to draw the star. One request
// for the whole list instead of one per track. Returns how many it wrote, or
// -1.
int qobuz_favorite_track_ids(long *out, int max);

// Albums, by textual id.
bool qobuz_favorite_add_album(const char *album_id);
bool qobuz_favorite_remove_album(const char *album_id);

bool qobuz_playlist_delete(long playlist_id);

bool qobuz_playlist_create(const char *name, long *out_id);
bool qobuz_playlist_add_track(long playlist_id, long track_id);

#endif /* QOBUZ_H */
