#ifndef ADDON_ADDONFS_H
#define ADDON_ADDONFS_H

#include <stdbool.h>
#include <stddef.h>

#include "src/addon/addon.h"

// Where an addon keeps its own files on the card.
//
// The rule these two functions exist to make easy: an addon writes under its own
// folder and nowhere else. Settings beside `addon.so` (that was already true
// everywhere), caches and indexes in `<addon>/cache/`, save files beside the
// settings. The card root and the shared `.local/` tree belong to the player --
// a service that put its caches there made a factory reset guess, made the USB
// export ask questions about a directory four addons claimed, and left files the
// user could not attribute to anything.
//
// The move is one-shot and deliberately conservative: the old path is only
// touched when the new one does not exist yet, and it is moved rather than
// copied, so a pull mid-way leaves exactly one of the two and the next start
// repeats the attempt.
//
// Order matters: call addon_migrate_path() BEFORE addon_cache_dir(). The helper
// treats an existing destination as "already migrated", and addon_cache_dir()
// creates exactly that destination -- the other way round, the migration becomes
// a silent no-op and the old tree stays forever.

// "<host->dir>/cache", created if it is not there. False when the card refuses
// to make the directory (a read-only card, or one that has just gone).
bool addon_cache_dir(const sonix_addon_host_t *host, char *out, size_t out_size);

// Moves a file or a whole directory from the shared tree into the addon's own
// folder. Does nothing when `new_path` already exists (the migration is done) or
// when `old_path` is not there (nothing to migrate). Creates the parent of
// `new_path`. False only on a real failure, which is logged.
bool addon_migrate_path(const char *old_path, const char *new_path);

#endif /* ADDON_ADDONFS_H */
