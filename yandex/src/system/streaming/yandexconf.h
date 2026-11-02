#ifndef YANDEXCONF_H
#define YANDEXCONF_H

#include <stdbool.h>
#include <stddef.h>

// The Yandex Music addon's own settings and keys, in a file on the card next to
// it.
//
// In the fork this came from, the address, the quality ceiling and the two
// application keys were the player's: the settings were [yandex] keys of
// device_config.ini and the keys were sealed into streaming-keys.bin with a
// key compiled into the binary (streamkeys.c). Neither is available to an addon
// -- an addon cannot open the sealed file, and the player's configuration is the
// player's (it is rewritten whole on every save and a worker writing to it races
// the interface for it) -- so both live here:
//
//     <card>/Addons/yandex/settings.ini     through addons/common/store.{h,c}
//
//   client_id       the OAuth application's id
//   client_secret   its secret
//   sign_key        the key get-file-info's signature is made with
//   client_header   the "X-Yandex-Music-Client: ..." line every request carries
//   base_url        https://api.music.yandex.net, or a mock
//   auth_url        https://oauth.yandex.ru, or a mock
//   quality         "lossless" (the default), "high", "normal"
//
// Nothing is compiled in and nothing is hard-coded: the four key values are the
// owner's, and a file that does not carry all four is "no keys", which is a
// state the addon shows rather than a failure (yandex_configured()).
//
// Migration: a setting the addon's file does not have yet is read from the old
// [yandex] section of device_config.ini once and written here, so a card written
// before the move keeps its quality and its address. The keys were never in
// device_config.ini -- they were sealed -- so there is nothing to import for
// them. Nothing is ever written back to the player's configuration.
//
// Thread contract: any thread. One mutex inside, held for the memory work and
// for the write; the card can be pulled mid-write and store_save() leaves the
// previous file whole.

#define YANDEXCONF_ID_MAX 128
#define YANDEXCONF_SECRET_MAX 160
#define YANDEXCONF_SIGN_MAX 256
#define YANDEXCONF_HEADER_MAX 256

// Points the settings at <dir>/settings.ini and reads them. `dir` is the
// addon's own folder (sonix_addon_host_t::dir); NULL or "" means "no card":
// every key then falls back and nothing is written.
void yandexconf_init(const char *dir);

// Reads the file again (the settings screen's "reload", after the owner put the
// keys on the card).
void yandexconf_reload(void);

// Copies the value of `key` into `out`, or `fallback` when the addon has none
// and the old section does not either.
void yandexconf_copy(const char *key, const char *fallback, char *out, size_t size);

// The same for one of the four keys, with the size the key may have.
bool yandexconf_key(const char *key, char *out, size_t size);

// True when all four keys are there and none is empty: what
// yandex_configured() answers with.
bool yandexconf_have_keys(void);

// Writes `key` and saves the file. An empty value removes the key.
void yandexconf_set(const char *key, const char *value);

// The addon's folder, "" when there is none.
const char *yandexconf_dir(void);

// The addon being unloaded: the folder and the settings read out of it are
// forgotten, nothing is written. Called from the addon's stop hook.
void yandexconf_stop(void);

// <dir>/<name>, false when there is no card.
bool yandexconf_path(const char *name, char *out, size_t size);

#endif /* YANDEXCONF_H */
