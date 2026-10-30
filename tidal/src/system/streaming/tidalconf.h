#ifndef STREAM_TIDALCONF_H
#define STREAM_TIDALCONF_H

#include <stdbool.h>
#include <stddef.h>

// Tidal's own settings and session, in files on the card next to the addon.
//
// They used to be the player's: the client pair came out of streaming-keys.ini
// (or the sealed .bin) and the tokens out of <card>/sonix/streaming-session.json,
// both read by src/system/streaming/streamkeys.c and handed to this addon
// through the export list. The player knows nothing about Tidal now, and both
// live here:
//
//     <card>/Addons/tidal/settings.ini   through addons/common/store.{h,c}
//     <card>/Addons/tidal/session.ini    its own file, see below
//
//   settings.ini:  client_id, client_secret   a pair that overrides the
//                                              built-in one
//                  migrated                    the old files have been read once
//   session.ini:   access_token, refresh_token, expires_at
//
// Why the session is not in the store: a Tidal token is an eight-to-nine
// hundred character JWT, and store_t holds 256 characters per value
// (addons/common/store.h). Widening that for one service would put the same
// size into every addon's copy of the store. So session.ini is written here,
// in the same `key = value` shape, with the same tmp-then-rename step that
// store_save() uses -- the card can be pulled mid-write and the previous
// session survives whole.
//
// Tidal's application pair is public and built into tidal.c; a pair here
// overrides it, which is what the keys file used to do. Migration, once: on the
// first run the addon reads what the player used to read --
// <card>/sonix/streaming.json and <card>/sonix/streaming-session.json for the
// tokens and the pair, then the [tidal] section of streaming-keys.ini (or the
// sealed .bin, through the one function streamkeys.h keeps for this) for a pair
// the card did not name -- writes both of these files, and marks settings.ini
// `migrated`. From then on the files here are the only source: a sign-out
// writes an empty session and does not bring the old one back.
//
// Thread contract: any thread. One mutex inside, held for the memory work and
// for the write.

#define TIDALCONF_ID_MAX 64
#define TIDALCONF_SECRET_MAX 128
#define TIDALCONF_TOKEN_MAX 2048

// Points the settings at <dir>/settings.ini and <dir>/session.ini, reads them,
// and migrates the old place on the first run. `dir` is the addon's own folder
// (sonix_addon_host_t::dir), `sd_root` the card's root, where the old files
// are; NULL or "" for either means "no card", and nothing is read or written.
void tidalconf_init(const char *dir, const char *sd_root);

// Reads both files again (and migrates, if that has not happened yet): the
// card may have been written while the player ran.
void tidalconf_reload(void);

// The client pair that overrides the built-in one. False when the addon has
// none, in which case the caller uses the pair compiled into tidal.c.
bool tidalconf_keys(char *client_id, size_t id_size, char *client_secret, size_t secret_size);

// The session, "" when signed out.
void tidalconf_session(char *access_token, size_t access_size, char *refresh_token, size_t refresh_size,
					   long *expires_at);

// Writes the session. An empty refresh token is a sign-out: the file is left
// with no keys in it, and the old place is not read again (see `migrated`).
void tidalconf_save_session(const char *access_token, const char *refresh_token, long expires_at);

// The addon's folder, "" when there is none.
const char *tidalconf_dir(void);

#endif /* STREAM_TIDALCONF_H */
