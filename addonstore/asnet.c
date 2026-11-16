#include "asnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/cardpace.h"

#include "src/system/core/json.h"
#include "src/system/core/sha256.h"
#include "src/system/net/http.h"

#define LIST_LIMIT (512 * 1024)
#define LIST_TIMEOUT_SECS 20
#define DOWNLOAD_TIMEOUT_SECS 30
#define CHUNK 32768

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

bool as_fetch_releases(const char *repo, as_release_t *out, int max, int *count, char *error, size_t errsz,
					   as_progress_t *pg) {
	*count = 0;
	if (!repo || !strchr(repo, '/')) {
		set_error(error, errsz, "the repository name is not owner/name");
		return false;
	}
	char url[256];
	snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases?per_page=1", repo);
	if (pg) {
		pg->total = 0;
		pg->done = 0;
	}

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
	int release = json_at(&doc, root, 0);
	if (release < 0) {
		json_free(&doc);
		free(body);
		set_error(error, errsz, "the repository has no published release");
		return false;
	}

	char tag[AS_TAG_MAX];
	if (!json_obj_str(&doc, release, "tag_name", tag, sizeof(tag))) {
		tag[0] = '\0';
	}
	int assets = json_get(&doc, release, "assets");
	int n = assets >= 0 ? json_len(&doc, assets) : 0;
	for (int i = 0; i < n && *count < max; i++) {
		int asset = json_at(&doc, assets, i);
		if (asset < 0) {
			continue;
		}
		as_release_t item;
		memset(&item, 0, sizeof(item));
		if (!json_obj_str(&doc, asset, "name", item.asset, sizeof(item.asset))) {
			continue;
		}
		as_id_from_asset(item.asset, item.id, sizeof(item.id));
		if (!item.id[0]) {
			continue;
		}
		snprintf(item.repo, sizeof(item.repo), "%s", repo);
		snprintf(item.tag, sizeof(item.tag), "%s", tag);
		item.size = json_obj_long(&doc, asset, "size", 0);
		json_obj_str(&doc, asset, "digest", item.digest, sizeof(item.digest));
		json_obj_str(&doc, asset, "browser_download_url", item.url, sizeof(item.url));
		if (!item.url[0] || strncmp(item.url, "https://", 8) != 0 || item.size <= 0) {
			continue;
		}
		out[(*count)++] = item;
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
	if (pg) {
		pg->done = 0;
		pg->total = total;
	}
	if (expect_size > 0 && stream.content_length > 0 && stream.content_length != expect_size) {
		set_error(error, errsz, "the server sends a different size than the release lists");
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
