#ifndef ADDONSTORE_ASINST_H
#define ADDONSTORE_ASINST_H

#include <stdbool.h>
#include <stddef.h>

#include "asdata.h"
#include "asnet.h"

// Putting an addon on the card, taking it off, and reading its README.
//
// An install is: the asset into the store's own cache, the zip unpacked into a
// hidden directory beside the target, and one rename to put it in place. The
// old version is moved aside first, not deleted, so a failure at any step
// leaves the addon that was there untouched -- the card is the only copy the
// user has.
//
// What is never done: unpacking straight into Addons/<id>/. A half-written
// addon.so there is a tile that cannot start, and the player reads that folder
// at start-up.

// Installs (or replaces) the addon the release names. `version` receives the
// manifest's version string of what was written, for the record file. The
// caller writes the record.
bool as_install(const as_model_t *m, const as_release_t *rel, as_progress_t *pg, char *version, size_t vsz,
				char *error, size_t errsz);

// Removes Addons/<id> and everything under it. Refuses the store's own folder:
// the library is mapped from it and the page is running inside it.
bool as_remove(const as_model_t *m, const char *id, char *error, size_t errsz);

// The README on the card for an installed addon, if it shipped one.
bool as_readme_local(const as_model_t *m, const char *id, char *out, size_t size);

// <addon>/cache/readme/<id>.txt -- what the store writes after pulling a
// README out of a release zip.
bool as_readme_cached(const as_model_t *m, const char *id, char *out, size_t size);

// Reads the release's addon.json without installing it: the tile's name and the
// page it belongs to, for the list. The zip is downloaded into the cache and
// deleted again; `rel->title`/`rel->section` are filled. Called once per
// published file -- the result is cached, so a release that does not change is
// not downloaded twice.
bool as_read_manifest(const as_model_t *m, as_release_t *rel, as_progress_t *pg, char *error, size_t errsz);

// Downloads the asset and writes its README.txt into the cache. False when the
// zip has no README.txt (the sentence says so), the download failed, or it was
// cancelled.
bool as_readme_fetch(const as_model_t *m, const as_release_t *rel, const char *id, as_progress_t *pg, char *error,
					 size_t errsz);

#endif /* ADDONSTORE_ASINST_H */
