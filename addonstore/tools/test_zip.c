// A host-side check for aszip.c: list a release zip, optionally write its
// entries out, and with --dump print one entry to stdout. Not compiled into the
// addon -- the Makefile skips everything under */tools/.
//
//   cc -O1 -Wall -I. -I../sonix-player/sonix-player -o /tmp/test_zip \
//      aszip.c tools/test_zip.c
//   /tmp/test_zip dino.zip --out /tmp/dino
//
// Compare the listing with `7z l` and the written files with the ones `7z x`
// produces; that is what the reader is verified against.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../aszip.h"

static int make_dirs(const char *path) {
	char buf[1200];
	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
				return -1;
			}
			*p = '/';
		}
	}
	if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
		return -1;
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s ZIP [--out DIR] [--dump NAME]\n", argv[0]);
		return 2;
	}
	const char *out = NULL;
	const char *dump = NULL;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
			out = argv[++i];
		} else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
			dump = argv[++i];
		}
	}

	aszip_t z;
	if (!aszip_open(&z, argv[1])) {
		fprintf(stderr, "not a readable zip: %s\n", argv[1]);
		return 1;
	}
	printf("%d entries, %d refused\n", z.count, z.bad);
	for (int i = 0; i < z.count; i++) {
		printf("%10ld %10ld method %d  %s\n", z.entry[i].comp_size, z.entry[i].uncomp_size, z.entry[i].method,
			   z.entry[i].name);
	}

	if (dump) {
		int i = aszip_find(&z, dump);
		if (i < 0) {
			fprintf(stderr, "no such entry: %s\n", dump);
			return 1;
		}
		size_t len = 0;
		uint8_t *data = aszip_read(&z, i, &len);
		if (!data) {
			fprintf(stderr, "cannot read: %s\n", dump);
			return 1;
		}
		fwrite(data, 1, len, stdout);
		free(data);
	}

	if (out) {
		make_dirs(out);
		for (int i = 0; i < z.count; i++) {
			size_t len = 0;
			uint8_t *data = aszip_read(&z, i, &len);
			if (!data) {
				fprintf(stderr, "cannot read: %s\n", z.entry[i].name);
				return 1;
			}
			char path[1200];
			snprintf(path, sizeof(path), "%s/%s", out, z.entry[i].name);
			char dir[1200];
			snprintf(dir, sizeof(dir), "%s", path);
			char *slash = strrchr(dir, '/');
			if (slash) {
				*slash = '\0';
				make_dirs(dir);
			}
			FILE *f = fopen(path, "wb");
			if (!f || fwrite(data, 1, len, f) != len) {
				fprintf(stderr, "cannot write: %s\n", path);
				free(data);
				return 1;
			}
			fclose(f);
			free(data);
		}
		printf("wrote %d files to %s\n", z.count, out);
	}

	aszip_close(&z);
	return z.bad ? 1 : 0;
}
