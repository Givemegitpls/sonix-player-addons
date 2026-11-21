#include "asnet.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/cardpace.h"

#include "src/system/core/json.h"
#include "src/system/core/sha256.h"
#include "src/system/net/http.h"

#define LIST_LIMIT (512 * 1024)
#define INDEX_LIMIT (256 * 1024)
#define LIST_TIMEOUT_SECS 20
#define DOWNLOAD_TIMEOUT_SECS 30
#define CHUNK 32768

// The stream the download worker is reading right now, if any, so that a stop
// can end it without waiting out the receive timeout: http_stream_wake() shuts
// the socket down, the pending recv() returns at once and the loop in
// as_download() notices progress.stop on its next chunk. One at a time is
// enough: the store runs one job per worker and one download per job.
static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t *live_stream;

static void live_set(http_stream_t *st) {
	pthread_mutex_lock(&live_lock);
	live_stream = st;
	pthread_mutex_unlock(&live_lock);
}

void as_cancel(void) {
	pthread_mutex_lock(&live_lock);
	if (live_stream) {
		// The stream stays valid -- its owner still closes it -- and every read
		// from here on fails, which is what ends the worker.
		http_stream_wake(live_stream);
	}
	pthread_mutex_unlock(&live_lock);
}

static void set_error(char *error, size_t size, const char *text) {
	if (error && size) {
		snprintf(error, size, "%s", text && text[0] ? text : "failed");
	}
}

// The API's own sentence when it refused: {"message": "API rate limit
// exceeded for ..."}. Without it a 403 reads as "the network failed", which
// sends the user looking at Wi-Fi.
static void api_error(const char *body, int status, char *error, size_t size) {
	if (body && body[0]) {
		json_doc_t doc;
		if (json_parse(body, &doc)) {
			char message[AS_ERROR_MAX];
			int root = json_root(&doc);
			if (json_obj_str(&doc, root, "message", message, sizeof(message)) && message[0]) {
				snprintf(error, size, "%s", message);
				json_free(&doc);
				return;
			}
			json_free(&doc);
		}
	}
	const char *why = http_last_error();
	if (why && why[0]) {
		snprintf(error, size, "%s", why);
	} else if (status == 403) {
		snprintf(error, size, "GitHub refused the request (HTTP 403, rate limit or blocked)");
	} else if (status == 404) {
		snprintf(error, size, "no such repository, or it has no release");
	} else if (status > 0) {
		snprintf(error, size, "GitHub answered HTTP %d", status);
	} else {
		snprintf(error, size, "no answer from GitHub (is Wi-Fi on?)");
	}
}

// The zip assets of one release: the asset's name is the addon's id, and the
// size, digest and URL come from the API. `listed` stays false -- the title and
// the section live inside the archive, so a row read this way has to open it
// later (as_read_manifest()).
static void entries_from_release(const json_doc_t *doc, int release, const char *repo, const char *tag,
								 as_release_t *out, int max, int *count) {
	int assets = json_get(doc, release, "assets");
	int n = assets >= 0 ? json_len(doc, assets) : 0;
	for (int i = 0; i < n && *count < max; i++) {
		int asset = json_at(doc, assets, i);
		if (asset < 0) {
			continue;
		}
		as_release_t item;
		memset(&item, 0, sizeof(item));
		if (!json_obj_str(doc, asset, "name", item.asset, sizeof(item.asset))) {
			continue;
		}
		as_id_from_asset(item.asset, item.id, sizeof(item.id));
		if (!item.id[0]) {
			continue;
		}
		bool seen = false;
		for (int k = 0; k < *count; k++) {
			if (strcmp(out[k].id, item.id) == 0) {
				seen = true;
			}
		}
		if (seen) {
			continue;
		}
		snprintf(item.repo, sizeof(item.repo), "%s", repo);
		snprintf(item.tag, sizeof(item.tag), "%s", tag);
		item.size = json_obj_long(doc, asset, "size", 0);
		json_obj_str(doc, asset, "digest", item.digest, sizeof(item.digest));
		json_obj_str(doc, asset, "browser_download_url", item.url, sizeof(item.url));
		if (!item.url[0] || strncmp(item.url, "https://", 8) != 0 || item.size <= 0) {
			continue;
		}
		out[(*count)++] = item;
	}
}

// The repository's own catalogue, if it publishes one: one file with every
// addon's row already in it, so nothing has to be downloaded and no API call is
// made. It is an asset of the compatibility release, at a URL that never
// changes.
static bool fetch_index(const char *repo, as_release_t *out, int max, int *count) {
	char url[320];
	snprintf(url, sizeof(url), "https://github.com/%s/releases/download/latest/index.json", repo);
	char *body = NULL;
	size_t body_len = 0;
	int status = 0;
	if (!http_get_ex(url, "Accept: application/json\r\n", false, &body, &body_len, INDEX_LIMIT, LIST_TIMEOUT_SECS,
					 &status)) {
		free(body);
		return false;
	}
	int n = body ? as_index_parse(body, out, max, repo) : -1;
	free(body);
	if (n <= 0) {
		return false;
	}
	*count = n;
	return true;
}

// The release the catalogue lives in: `index.json` beside a byte copy of every
// current archive, rebuilt by the CI on a fresh commit, which is what keeps it
// the newest release in the list. Read when that `index.json` cannot be had --
// deleted by hand, a partial upload -- and cheaper than the walk below in every
// way: one request for the whole list, no release ordering to depend on.
static bool fetch_latest(const char *repo, as_release_t *out, int max, int *count) {
	char url[256];
	snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/tags/latest", repo);
	char *body = NULL;
	size_t body_len = 0;
	int status = 0;
	if (!http_get_ex(url, "Accept: application/vnd.github+json\r\n", false, &body, &body_len, LIST_LIMIT,
					 LIST_TIMEOUT_SECS, &status)) {
		free(body);
		return false;
	}
	json_doc_t doc;
	if (!body || !json_parse(body, &doc)) {
		free(body);
		return false;
	}
	int root = json_root(&doc);
	char tag[AS_TAG_MAX];
	bool usable = root >= 0 && json_obj_str(&doc, root, "tag_name", tag, sizeof(tag)) && strcmp(tag, "latest") == 0;
	if (!usable) {
		json_free(&doc);
		free(body);
		return false;
	}
	int before = *count;
	entries_from_release(&doc, root, repo, tag, out, max, count);
	json_free(&doc);
	free(body);
	return *count > before;
}

bool as_fetch_releases(const char *repo, as_release_t *out, int max, int *count, char *error, size_t errsz,
					   as_progress_t *pg) {
	*count = 0;
	if (!repo || !strchr(repo, '/')) {
		set_error(error, errsz, "the repository name is not owner/name");
		return false;
	}
	if (pg) {
		pg->total = 0;
		pg->done = 0;
	}
	// The catalogue first: a repository that publishes one is answered with a
	// single GET, where the API path below costs a call per repository and then
	// a download per addon just to learn its title.
	if (fetch_index(repo, out, max, count)) {
		return true;
	}
	// Then the release it lives in: one API call, every current archive, and no
	// dependence on which release the list puts first.
	if (fetch_latest(repo, out, max, count)) {
		return true;
	}

	char url[256];
	snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases?per_page=100", repo);

	char *body = NULL;
	size_t body_len = 0;
	int status = 0;
	bool ok = http_get_ex(url, "Accept: application/vnd.github+json\r\n", true, &body, &body_len, LIST_LIMIT,
						  LIST_TIMEOUT_SECS, &status);
	if (!ok) {
		api_error(body, status, error, errsz);
		free(body);
		return false;
	}
	if (pg && pg->stop) {
		free(body);
		set_error(error, errsz, "cancelled");
		return false;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		set_error(error, errsz, "the release list is not JSON");
		return false;
	}
	int root = json_root(&doc);
	int releases = json_len(&doc, root);
	if (releases <= 0) {
		json_free(&doc);
		free(body);
		set_error(error, errsz, "the repository has no published release");
		return false;
	}

	// The API lists releases newest first, and one tag per addon means the
	// newest release names a single addon: reading only the first one would
	// show one addon and forget the rest. So every release is read and the
	// first (newest) occurrence of an id wins -- which is also what the
	// catalogue says, just slower and with a download per addon later.
	for (int r = 0; r < releases && *count < max; r++) {
		int release = json_at(&doc, root, r);
		if (release < 0) {
			continue;
		}
		if (json_obj_bool(&doc, release, "draft", false) || json_obj_bool(&doc, release, "prerelease", false)) {
			continue;
		}
		char tag[AS_TAG_MAX];
		if (!json_obj_str(&doc, release, "tag_name", tag, sizeof(tag))) {
			tag[0] = '\0';
		}
		// `latest` is the compatibility release read above, and a `store`
		// release may still be left over from the builds that published the
		// catalogue separately: neither is a release of its own.
		if (strcmp(tag, "store") == 0 || strcmp(tag, "latest") == 0) {
			continue;
		}
		entries_from_release(&doc, release, repo, tag, out, max, count);
	}
	json_free(&doc);
	free(body);
	return true;
}

bool as_download(const char *url, const char *path, long expect_size, const char *expect_sha, as_progress_t *pg,
				 char *error, size_t errsz) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		set_error(error, errsz, "cannot create the file on the card");
		return false;
	}

	http_stream_t stream;
	memset(&stream, 0, sizeof(stream));
	stream.fd = -1;
	if (!http_stream_open(&stream, url, DOWNLOAD_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		set_error(error, errsz, why && why[0] ? why : "the download did not start");
		fclose(f);
		unlink(path);
		return false;
	}

	long total = stream.content_length > 0 ? stream.content_length : expect_size;
	live_set(&stream);
	if (pg) {
		pg->done = 0;
		pg->total = total;
	}
	if (expect_size > 0 && stream.content_length > 0 && stream.content_length != expect_size) {
		set_error(error, errsz, "the server sends a different size than the release lists");
		live_set(NULL);
		http_stream_close(&stream);
		fclose(f);
		unlink(path);
		return false;
	}

	sha256_ctx sha;
	sha256_init(&sha);
	cardpace_t pace;
	cardpace_reset(&pace);

	char chunk[CHUNK];
	long done = 0;
	bool ok = true;
	while (ok) {
		if (pg && pg->stop) {
			set_error(error, errsz, "cancelled");
			ok = false;
			break;
		}
		int want = (int)sizeof(chunk);
		if (total > 0 && total - done < want) {
			want = (int)(total - done);
		}
		if (want <= 0) {
			break;
		}
		int got = http_stream_read(&stream, chunk, want);
		if (got <= 0) {
			set_error(error, errsz, total > 0 && done < total ? "the connection ended before the file did"
															  : "the download stopped");
			ok = false;
			break;
		}
		if (fwrite(chunk, 1, (size_t)got, f) != (size_t)got) {
			set_error(error, errsz, "write to the card failed (is it full?)");
			ok = false;
			break;
		}
		sha256_update(&sha, chunk, (size_t)got);
		done += got;
		cardpace_note(&pace, done);
		if (pg) {
			pg->done = done;
		}
	}
	live_set(NULL);
	http_stream_close(&stream);

	if (ok && expect_size > 0 && done != expect_size) {
		set_error(error, errsz, "the file is not the size the release lists");
		ok = false;
	}
	if (ok && expect_sha && strncmp(expect_sha, "sha256:", 7) == 0 && strlen(expect_sha + 7) == 64) {
		char hex[SHA256_HEX_LEN];
		sha256_final_hex(&sha, hex);
		if (strcasecmp(hex, expect_sha + 7) != 0) {
			set_error(error, errsz, "the download does not match the published sha256");
			ok = false;
		}
	}
	if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
		if (ok) {
			set_error(error, errsz, "write to the card failed");
		}
		ok = false;
	}
	if (fclose(f) != 0) {
		ok = false;
	}
	if (!ok) {
		unlink(path);
	}
	return ok;
}
