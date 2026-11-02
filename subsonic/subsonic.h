#ifndef SUBSONIC_H
#define SUBSONIC_H

#include <stdbool.h>
#include <stddef.h>

// A client for the Subsonic API, as the OpenSubsonic servers speak it:
// Navidrome, Gonic, Airsonic-Advanced, Ampache, LMS. The user's own music on the
// user's own server, which makes this the one streaming service with no
// application keys, no subscription and no region.
//
// What is used of the protocol, and why only that:
//
//   * JSON (f=json), API version 1.16.1, client name "sonix". JSON rather than
//     the original XML because json.c already exists and every OpenSubsonic
//     server answers it;
//   * token authentication: t = md5(password + salt), s = salt. The salt is
//     made once at sign-in and kept, with the token, in that server's own file
//     on the card -- the password itself is never written anywhere. A fixed
//     salt is what the servers accept and is also what keeps cover URLs stable,
//     so the list covers cached by URL (listart.c) are found again next time;
//   * the old plain-password form (p=enc:<hex>) only when the server refuses
//     tokens (error 41, e.g. an LDAP-backed account). That password then does
//     have to be kept -- hex-encoded, which is obfuscation, not protection --
//     and the settings page says so;
//   * stream: format=raw by default (the file exactly as on the server), or a
//     transcode chosen in the settings for a slow network.
//
// Ids are strings: each server invents its own. They are only meaningful on
// the server that made them, hence subsonic_track_key().
//
// Every function that talks to the network blocks. Call them from a worker
// thread, never from the GUI thread -- the same rule as http.h and qobuz.h.

#define SUBSONIC_ID_MAX 64
#define SUBSONIC_TITLE_MAX 200
#define SUBSONIC_NAME_MAX 128
#define SUBSONIC_URL_MAX 768
#define SUBSONIC_SERVER_MAX 256

// Servers: one file each under <dir>/servers/, 16 is more accounts than a
// player is likely to have, and the list is read whole on every page that
// needs it.
#define SUBSONIC_HOSTS_MAX 16
#define SUBSONIC_SLUG_MAX 32
#define SUBSONIC_LABEL_MAX 64

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char title[SUBSONIC_TITLE_MAX];
	char artist[SUBSONIC_NAME_MAX];
	char album[SUBSONIC_TITLE_MAX];
	char album_id[SUBSONIC_ID_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	char suffix[12]; // "flac": the file's own, i.e. what format=raw sends
	int duration;	 // seconds
	int track_number;
	int bit_rate; // kbps, 0 when unknown
	bool starred;
} subsonic_track_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char title[SUBSONIC_TITLE_MAX];
	char artist[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	int year;
	int song_count;
} subsonic_album_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char name[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	int album_count;
} subsonic_artist_t;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char name[SUBSONIC_TITLE_MAX];
	char owner[SUBSONIC_NAME_MAX];
	char cover_id[SUBSONIC_ID_MAX];
	int song_count;
} subsonic_playlist_t;

// ---------------------------------------------------------------------------
// Servers
//
// One account per server, one file per account, so a player with two servers in
// the house (or two names on one) does not have to sign out to change which one
// it is looking at:
//
//     <dir>/servers/<slug>.ini   label, server, user, salt, token, password_hex, quality
//     <dir>/servers/active       the slug of the one in use, one line of text
//
// `slug` is the server's host made into a file name -- "192.168.1.10-4533" for
// "http://192.168.1.10:4533", with a number appended when two need it -- and it
// never changes hands: the file this addon has always written, session.ini, is
// moved to servers/default.ini the first time this version runs.
//
// Everything above this section speaks to the *active* server, and the session
// is reloaded when another is chosen: lists, stream URLs, cover URLs and the
// star all mean the server the user picked.
// ---------------------------------------------------------------------------

// Reads the saved servers out of <dir>/servers/ once, from init(), with
// host->dir, and loads the active one. An older <dir>/session.ini is moved into
// place first. With no card to write to, the session is kept in memory and
// subsonic_hosts_note() says so.
void subsonic_load(const char *dir);

bool subsonic_logged_in(void);

// "alice @ music.example.org", for the settings page. Empty when signed out.
void subsonic_display_name(char *out, size_t size);

// The active server's base URL as saved ("https://music.example.org"). Empty
// when there is no server.
void subsonic_server(char *out, size_t size);

// The user name as saved. Empty when signed out. For the sign-in form, which
// starts with what is already known so that signing in again is a password.
void subsonic_user_name(char *out, size_t size);

// The host a server URL points at, without the scheme or the path:
// "music.example.org", "192.168.1.10:4533". A row's short name for a server.
void subsonic_server_host(const char *server, char *out, size_t size);

// Rescans <dir>/servers/ and sorts the result by label. A dozen small files,
// on the GUI thread.
void subsonic_hosts_refresh(void);
int subsonic_hosts_count(void);

// One server for the list, by position. False when `index` is past the end.
bool subsonic_host_at(int index, char *label, size_t label_size, char *server, size_t server_size, char *user,
					  size_t user_size);
bool subsonic_host_slug_at(int index, char *out, size_t size);

// The slug of the server in use, false when there is none.
bool subsonic_host_active_slug(char *out, size_t size);

// Makes `slug` the active server: reloads the session and the quality from its
// file. False when there is no such server, and the current one is kept.
bool subsonic_host_select(const char *slug);

// Deletes that server's file. When it was the active one, the first remaining
// server is activated, or none. False when the file could not be removed.
bool subsonic_host_forget(const char *slug);

// Blocks. Checks the server with ping and, only when it answers, writes the
// server, user, salt and token into the file of `slug` -- NULL or "" signs in
// as a new server, whose file is named after its host. The server becomes the
// active one. `server` may be given without a scheme ("192.168.1.10:4533"),
// which means http://. A failed sign-in leaves the saved file as it was.
bool subsonic_login_host(const char *slug, const char *server, const char *user, const char *password);

// Signs in as a new server: subsonic_login_host(NULL, ...), for a caller that
// does not choose the file -- the address names it.
bool subsonic_login(const char *server, const char *user, const char *password);

// Signs out the active server: forgets its token and keeps its server and name,
// so signing back in is mostly typing the password again. The other servers are
// untouched.
void subsonic_logout(void);

// Drops the session, the servers and the settings from memory without writing
// anything, for the addon's stop(): the files on the card stay as they are, so
// the copy loaded next still finds them. The session's own lock is re-used, not
// rebuilt.
void subsonic_forget(void);

// The last thing about the servers folder the user should be told when it is
// not the failure of a request ("signed in, but the card would not keep it").
// Empty when there is nothing to say.
void subsonic_hosts_note(char *out, size_t size);

// Why the last call failed, fit to display. Per thread, like http_last_error().
const char *subsonic_last_error(void);

// ---------------------------------------------------------------------------
// Streaming quality
// ---------------------------------------------------------------------------

typedef enum {
	SUBSONIC_QUALITY_ORIGINAL = 0, // format=raw: the file as it is on the server
	SUBSONIC_QUALITY_MP3_320,
	SUBSONIC_QUALITY_MP3_192,
	SUBSONIC_QUALITY_OPUS_128,
	SUBSONIC_QUALITY_COUNT,
} subsonic_quality_t;

// Per server: a player on a slow link wants 192 for the house server and raw
// for the one it syncs with, and the choice travels with the account.
int subsonic_get_quality(void);

// Stores the choice in the active server's file. With no file to write to it
// holds for this run only.
void subsonic_set_quality(int quality);

// The suffix the file will have at the current quality: the track's own for
// the original, the transcode's otherwise. Gives the cache name before asking.
const char *subsonic_expected_suffix(const subsonic_track_t *track);

// ---------------------------------------------------------------------------
// Requests. Lists return how many items they wrote, or -1 on error. All block.
// `offset` skips that many entries, for paging.
// ---------------------------------------------------------------------------

bool subsonic_ping(void);

int subsonic_artists(int offset, subsonic_artist_t *out, int max);
int subsonic_artist_albums(const char *artist_id, int offset, subsonic_album_t *out, int max);
int subsonic_album_tracks(const char *album_id, int offset, subsonic_track_t *out, int max);

// `type` is getAlbumList2's own: "newest", "recent", "random", "frequent",
// "starred", "alphabeticalByName".
int subsonic_album_list(const char *type, int offset, subsonic_album_t *out, int max);

int subsonic_search_tracks(const char *query, int offset, subsonic_track_t *out, int max);
int subsonic_search_albums(const char *query, int offset, subsonic_album_t *out, int max);
int subsonic_search_artists(const char *query, int offset, subsonic_artist_t *out, int max);

int subsonic_playlists(int offset, subsonic_playlist_t *out, int max);
int subsonic_playlist_tracks(const char *playlist_id, int offset, subsonic_track_t *out, int max);

// getRandomSongs: `max` tracks the server picks at random (no paging: each
// call is a new draw).
int subsonic_random_tracks(subsonic_track_t *out, int max);
// Is the server there: a timed ping (`ms`) and, when the server says it, how
// many tracks it holds (`tracks`, -1 when not said). False when the ping
// fails. Blocking: a worker thread's.
bool subsonic_status(int *tracks, int *ms);
int subsonic_starred_tracks(int offset, subsonic_track_t *out, int max);
int subsonic_starred_albums(int offset, subsonic_album_t *out, int max);

// The ids of every starred song, for the star's mirror. Writes up to `max`
// ids of SUBSONIC_ID_MAX characters each into `out`; returns how many, or -1.
int subsonic_starred_ids(char (*out)[SUBSONIC_ID_MAX], int max);

// star / unstar. `album` = the id is an album's, not a song's.
bool subsonic_star(const char *id, bool album, bool want);

// "Now playing" (submission false, at the start) and the play count
// (submission true, once listened to).
bool subsonic_scrobble(const char *song_id, bool submission);

bool subsonic_playlist_add(const char *playlist_id, const char *song_id);
bool subsonic_playlist_create(const char *name, const char *song_id);

// ---------------------------------------------------------------------------
// URLs. No network: they are built, not fetched. Empty when signed out.
// ---------------------------------------------------------------------------

// A file-name-safe name for one song of the active server: FNV-1a over
// (server, song id), sixteen hex digits. The id itself is kept in the cache's
// sidecar, because a Subsonic id is the server's own string ("so-45", a UUID)
// and that is not a name to put on a FAT card -- and because two servers number
// their songs independently.
void subsonic_track_key(const char *song_id, char *out, size_t size);

// The audio, at the current quality.
void subsonic_stream_url(const char *song_id, char *out, size_t size);

// The cover, `px` pixels square (300 for lists, 600 for the player). Stable for
// the session's salt, so it can name a cache file.
void subsonic_cover_url(const char *cover_id, int px, char *out, size_t size);

#endif /* SUBSONIC_H */
