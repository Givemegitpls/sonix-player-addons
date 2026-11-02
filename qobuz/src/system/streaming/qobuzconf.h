#ifndef STREAM_QOBUZCONF_H
#define STREAM_QOBUZCONF_H

#include <stdbool.h>
#include <stddef.h>

// Qobuz's own settings and session, in files on the card next to the addon.
//
// They used to be the player's: the application pair came out of
// streaming-keys.ini (or the sealed .bin), and the token out of
// <card>/sonix/streaming-session.json, both read by src/system/streaming/
// streamkeys.c and handed to this addon through the export list. The player
// knows nothing about Qobuz now, and both live here:
//
//     <card>/Addons/qobuz/settings.ini   through addons/common/store.{h,c}
//     <card>/Addons/qobuz/session.ini    through addons/common/store.{h,c}
//
//   settings.ini:  app_id, app_secret   the application pair
//                  by_user             1 when the pair is the owner's (a pair
//                                      named in a file), never refreshed
//                  creds_at            when the pair was derived from the web
//                                      player, 0 when it was not
//                  migrated           the old files have been read once
//   session.ini:   token, user          the account
//                  signed_out          the token file is not to be read again
//                                      until someone signs in (see below)
//
// The pair is Qobuz's, not this project's: when the addon has none it derives
// one from the web player (qobuzkey.c) and keeps it here, which is why
// `by_user` exists at all -- a derived pair goes stale and is taken again, the
// owner's does not.
//
// Migration, once: on the first run the addon reads what the player used to
// read -- <card>/sonix/streaming.json and <card>/sonix/streaming-session.json
// for the token and the pair, then the [qobuz] section of streaming-keys.ini
// (or the sealed .bin, through the one function streamkeys.h keeps for this)
// for a pair the card did not name -- writes both of these files, and marks
// settings.ini `migrated`. From then on the files here are the only source: a
// sign-out writes an empty session and does not bring the old one back.
//
// Thread contract: any thread. One mutex inside, held for the memory work and
// for the write; the card can be pulled mid-write and store_save() leaves the
// previous file whole.

#define QOBUZCONF_ID_MAX 64
#define QOBUZCONF_SECRET_MAX 128
#define QOBUZCONF_TOKEN_MAX 256
#define QOBUZCONF_USER_MAX 128

// Points the settings at <dir>/settings.ini and <dir>/session.ini, reads them,
// and migrates the old place on the first run. `dir` is the addon's own folder
// (sonix_addon_host_t::dir), `sd_root` the card's root, where the old files
// are; NULL or "" for either means "no card", and nothing is read or written.
void qobuzconf_init(const char *dir, const char *sd_root);

// Reads both files again (and migrates, if that has not happened yet): the
// card may have been written while the player ran.
void qobuzconf_reload(void);

// The application pair, "" when there is none. `by_user` and `creds_at` are
// what settings.ini says about where it came from.
void qobuzconf_keys(char *app_id, size_t id_size, char *app_secret, size_t secret_size, bool *by_user,
					long *creds_at);

// Writes the pair and where it came from. A derived pair is saved with
// `by_user` false and the time it was taken.
void qobuzconf_save_creds(const char *app_id, const char *app_secret, long at, bool by_user);

// The account, "" when signed out.
void qobuzconf_session(char *token, size_t token_size, char *user, size_t user_size);

// Writes the account. An empty token is a sign-out: the keys are removed from
// the file, `signed_out` is set, and the old place is not read again (see
// `migrated`). False when the card would not take it (read-only, full, gone):
// the session in memory is still the one this run uses.
bool qobuzconf_save_session(const char *token, const char *user);

// True when the last thing written here was a sign-out. The token file on the
// card is the account, so without this a sign-out would last exactly until the
// next start; with it, coming back is the "Sign in from the card" row -- which
// ignores this -- and not a restart of the player.
bool qobuzconf_signed_out(void);

// The addon's folder, "" when there is none.
const char *qobuzconf_dir(void);

#endif /* STREAM_QOBUZCONF_H */
