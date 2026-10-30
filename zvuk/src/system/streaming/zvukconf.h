#ifndef ZVUKCONF_H
#define ZVUKCONF_H

#include <stdbool.h>
#include <stddef.h>

// The addon's settings, in a file on the card next to it.
//
// In the fork this came from these were [zvuk] keys of device_config.ini: the
// base address (the simulator points it at tools/zvuk_mock.py), the browser's
// User-Agent, the quality ceiling and the id the connection check uses. That
// file belongs to the player -- it is rewritten whole on every save and a
// worker writing to it races the interface for it -- so an addon keeps its own:
//
//     <card>/Addons/zvuk/settings.ini     through addons/common/store.{h,c}
//
// which is what docs/ADDONS.md, "Where an addon keeps its settings", asks for,
// and which also means a card carries its Zvuk settings to another player.
//
// Migration: a key the addon's file does not have yet is read from the old
// [zvuk] section of device_config.ini once and written here, so a card written
// before the move keeps its quality and its address. Nothing is ever written
// back to the player's configuration.
//
// Thread contract: any thread. One mutex inside, held for the memory work and
// for the write; the card can be pulled mid-write and store_save() leaves the
// previous file whole.

// Points the settings at <dir>/settings.ini and reads them. `dir` is the
// addon's own folder (sonix_addon_host_t::dir); NULL or "" means "no card":
// every key then falls back and nothing is written.
void zvukconf_init(const char *dir);

// Copies the value of `key` into `out`, or `fallback` when the addon has none
// and the old section does not either.
void zvukconf_copy(const char *key, const char *fallback, char *out, size_t size);

// Writes `key` and saves the file. An empty value removes the key.
void zvukconf_set(const char *key, const char *value);

// The addon's folder, "" when there is none.
const char *zvukconf_dir(void);

// <dir>/<name>, false when there is no card.
bool zvukconf_path(const char *name, char *out, size_t size);

#endif /* ZVUKCONF_H */
