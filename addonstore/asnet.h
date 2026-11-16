#ifndef ADDONSTORE_ASNET_H
#define ADDONSTORE_ASNET_H

#include <stdbool.h>
#include <stddef.h>

#include "asdata.h"

// The two network calls the store makes, both on the worker.
//
// GitHub answers the release list as JSON at api.github.com and the asset
// itself with a 302 to objects.githubusercontent.com; the player's http client
// follows both and speaks https, so nothing here knows about TLS.
//
// The API is anonymous, and anonymous GitHub allows sixty requests an hour per
// address. That is why the page shows the cached list first and refreshes at
// most once per open, not per row.

// What the UI polls while a job runs. `stop` is set by the addon's stop() and
// by a cancel on the page; the worker checks it between chunks (a few hundred
// milliseconds at the paced rate).
typedef struct {
	volatile long done;
	volatile long total;
	volatile bool stop;
} as_progress_t;

#define AS_ERROR_MAX 192

// The latest release of `repo`, one entry per .zip asset. `count` is what was
// written; entries past `max` are dropped. False on a network/API failure, with
// `error` carrying the sentence the page should show (the API's own "message"
// for a refusal such as the rate limit).
bool as_fetch_releases(const char *repo, as_release_t *out, int max, int *count, char *error, size_t errsz,
					   as_progress_t *pg);

// Downloads `url` into `path` (a .part file the caller renames), paced to the
// card. Verifies the size GitHub published and, when `expect_sha` is
// "sha256:<hex>", the digest. False leaves `path` behind for the caller to
// unlink, with `error` filled.
bool as_download(const char *url, const char *path, long expect_size, const char *expect_sha, as_progress_t *pg,
				 char *error, size_t errsz);

#endif /* ADDONSTORE_ASNET_H */
