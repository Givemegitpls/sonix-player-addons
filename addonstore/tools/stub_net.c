// The network, stubbed, for tools/test_store.c. The release list is canned; the
// asset is the zip named by AS_TEST_ZIP. Nothing here is compiled into the
// addon -- the Makefile skips */tools/.

#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/sha256.h"
#include "src/system/net/http.h"

static const char *zip_path(void) {
	const char *p = getenv("AS_TEST_ZIP");
	return p ? p : "";
}

static long zip_size(void) {
	struct stat st;
	return stat(zip_path(), &st) == 0 ? (long)st.st_size : 0;
}

// A whole file into memory: the canned release list, and the catalogue the
// index URL is answered with.
static bool read_all(const char *path, char **out, size_t *out_len) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *raw = len >= 0 ? malloc((size_t)len + 1) : NULL;
	if (!raw || fread(raw, 1, (size_t)len, f) != (size_t)len) {
		free(raw);
		fclose(f);
		return false;
	}
	fclose(f);
	raw[len] = '\0';
	*out = raw;
	if (out_len) {
		*out_len = (size_t)len;
	}
	return true;
}

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out, size_t *out_len,
				 size_t limit, int timeout_secs, int *status_out) {
	(void)extra_headers;
	(void)want_error_body;
	(void)limit;
	(void)timeout_secs;

	const char *index_file = getenv("AS_TEST_INDEX");
	if (index_file && index_file[0] && url && strstr(url, "/releases/download/latest/index.json")) {
		if (!read_all(index_file, out, out_len)) {
			return false;
		}
		if (status_out) {
			*status_out = 200;
		}
		return true;
	}

	// The compatibility release itself, for the case the catalogue file is
	// missing: {"tag_name":"latest","assets":[...]}.
	const char *bundle_file = getenv("AS_TEST_BUNDLE");
	if (bundle_file && bundle_file[0] && url && strstr(url, "/releases/tags/latest")) {
		if (!read_all(bundle_file, out, out_len)) {
			return false;
		}
		if (status_out) {
			*status_out = 200;
		}
		return true;
	}

	const char *json_file = getenv("AS_TEST_JSON");
	if (json_file && json_file[0]) {
		if (!read_all(json_file, out, out_len)) {
			return false;
		}
		if (status_out) {
			*status_out = 200;
		}
		return true;
	}

	char *data = malloc(4096);
	if (!data) {
		return false;
	}
	sha256_ctx sha;
	sha256_init(&sha);
	FILE *f = fopen(zip_path(), "rb");
	char buf[4096];
	size_t n;
	while (f && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
		sha256_update(&sha, buf, n);
	}
	if (f) {
		fclose(f);
	}
	char hex[SHA256_HEX_LEN];
	sha256_final_hex(&sha, hex);

	int len = snprintf(data, 4096,
					   "[{\"tag_name\":\"9.9.9\",\"assets\":[{\"name\":\"dino.zip\",\"size\":%ld,"
					   "\"digest\":\"sha256:%s\",\"browser_download_url\":\"https://test.invalid/dino.zip\"}]}]",
					   zip_size(), hex);
	*out = data;
	if (out_len) {
		*out_len = (size_t)(len > 0 ? len : 0);
	}
	if (status_out) {
		*status_out = 200;
	}
	return true;
}

bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len, size_t limit,
				  int timeout_secs, int *status_out) {
	(void)req;
	return http_get_ex(url, NULL, false, out, out_len, limit, timeout_secs, status_out);
}

const char *http_last_error(void) {
	return NULL;
}

bool http_stream_open(http_stream_t *st, const char *url, int timeout_secs) {
	(void)url;
	(void)timeout_secs;
	int fd = open(zip_path(), O_RDONLY);
	if (fd < 0) {
		return false;
	}
	st->fd = fd;
	st->content_length = zip_size();
	return true;
}

int http_stream_read(http_stream_t *st, void *buf, int len) {
	return (int)read(st->fd, buf, (size_t)len);
}

void http_stream_close(http_stream_t *st) {
	if (st->fd >= 0) {
		close(st->fd);
		st->fd = -1;
	}
}

const char *lang_current(void) {
	return "English";
}
