#include "aszip.h"

#include <stdlib.h>
#include <string.h>

// miniz's inflate. It is already compiled into every addon by common/gunzip.c
// (which carries tinfl_impl.inc), so this file only names it: including the
// implementation here as well would define every tinfl symbol twice at link
// time. The player's own copy is not exported and is not what resolves this.
extern void *tinfl_decompress_mem_to_heap(const void *src, size_t src_len, size_t *out_len, int flags);

#define EOCD_SIG 0x06054b50u
#define CENTRAL_SIG 0x02014b50u
#define LOCAL_SIG 0x04034b50u
#define EOCD_MIN 22
#define EOCD_SCAN 65557 // the fixed part plus the longest possible comment

static bool read_at(FILE *f, long off, void *buf, size_t len) {
	if (off < 0) {
		return false;
	}
	if (fseek(f, off, SEEK_SET) != 0) {
		return false;
	}
	return fread(buf, 1, len, f) == len;
}

static unsigned rd16(const uint8_t *p) {
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long rd32(const uint8_t *p) {
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
		   ((unsigned long)p[3] << 24);
}

// A name that will not escape the directory it is joined onto. Plain bytes: a
// UTF-8 sequence has no '/' or '\\' inside it.
static bool name_safe(const char *name) {
	if (!name[0] || name[0] == '/') {
		return false;
	}
	for (const char *p = name; *p; p++) {
		if (*p == '\\' || *p == ':' || (unsigned char)*p < 0x20) {
			return false;
		}
	}
	const char *p = name;
	while (*p) {
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);
		if (len == 0 || (len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) {
			return false;
		}
		if (!slash) {
			break;
		}
		p = slash + 1;
	}
	return true;
}

// "repo-tag/addon.json" and "repo-tag/icon.png" become "addon.json" and
// "icon.png" when every entry sits under the same first component. A release
// zip from this project has no directory at all, and is left alone.
static void strip_common_dir(aszip_t *z) {
	if (z->count <= 0) {
		return;
	}
	char prefix[ASZIP_NAME_MAX];
	const char *slash = strchr(z->entry[0].name, '/');
	if (!slash) {
		return;
	}
	size_t plen = (size_t)(slash - z->entry[0].name);
	if (plen == 0 || plen >= sizeof(prefix)) {
		return;
	}
	memcpy(prefix, z->entry[0].name, plen);
	prefix[plen] = '\0';

	for (int i = 1; i < z->count; i++) {
		const char *n = z->entry[i].name;
		if (strncmp(n, prefix, plen) != 0 || n[plen] != '/') {
			return;
		}
	}

	for (int i = 0; i < z->count; i++) {
		const char *n = z->entry[i].name + plen + 1;
		// An entry that was the directory itself has nothing left to name.
		if (!n[0]) {
			z->bad++;
			continue;
		}
		memmove(z->entry[i].name, n, strlen(n) + 1);
	}
}

bool aszip_open(aszip_t *z, const char *path) {
	memset(z, 0, sizeof(*z));
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long size = ftell(f);
	if (size < EOCD_MIN) {
		fclose(f);
		return false;
	}

	long scan = size < EOCD_SCAN ? size : EOCD_SCAN;
	long start = size - scan;
	uint8_t *tail = malloc((size_t)scan);
	if (!tail) {
		fclose(f);
		return false;
	}
	if (!read_at(f, start, tail, (size_t)scan)) {
		free(tail);
		fclose(f);
		return false;
	}

	long eocd = -1;
	for (long i = scan - EOCD_MIN; i >= 0; i--) {
		if (rd32(tail + i) == EOCD_SIG) {
			eocd = start + i;
			break;
		}
	}
	free(tail);
	if (eocd < 0) {
		fclose(f);
		return false;
	}

	uint8_t hdr[EOCD_MIN];
	if (!read_at(f, eocd, hdr, sizeof(hdr))) {
		fclose(f);
		return false;
	}
	unsigned entries = rd16(hdr + 10);
	unsigned long cd_size = rd32(hdr + 12);
	unsigned long cd_off = rd32(hdr + 16);
	if (entries == 0 || entries > ASZIP_MAX_ENTRIES || cd_size < 4 || cd_off > (unsigned long)size) {
		fclose(f);
		return false;
	}
	uint8_t *cd = malloc(cd_size);
	if (!cd) {
		fclose(f);
		return false;
	}
	if (!read_at(f, (long)cd_off, cd, cd_size)) {
		free(cd);
		fclose(f);
		return false;
	}

	unsigned long pos = 0;
	for (unsigned i = 0; i < entries && pos + 46 <= cd_size; i++) {
		if (rd32(cd + pos) != CENTRAL_SIG) {
			break;
		}
		unsigned method = rd16(cd + pos + 10);
		unsigned long comp = rd32(cd + pos + 20);
		unsigned long uncomp = rd32(cd + pos + 24);
		unsigned nlen = rd16(cd + pos + 28);
		unsigned elen = rd16(cd + pos + 30);
		unsigned clen = rd16(cd + pos + 32);
		unsigned long local = rd32(cd + pos + 42);
		if (pos + 46 + nlen > cd_size || nlen == 0 || nlen >= ASZIP_NAME_MAX) {
			break;
		}
		char name[ASZIP_NAME_MAX];
		memcpy(name, cd + pos + 46, nlen);
		name[nlen] = '\0';

		if (name[nlen - 1] != '/') { // a directory entry carries nothing
			aszip_entry_t *e = &z->entry[z->count];
			bool known = method == 0 || method == 8;
			if (!known || comp > ASZIP_ENTRY_MAX || uncomp > ASZIP_ENTRY_MAX || !name_safe(name)) {
				z->bad++;
			} else {
				snprintf(e->name, sizeof(e->name), "%s", name);
				e->comp_size = (long)comp;
				e->uncomp_size = (long)uncomp;
				e->method = (int)method;
				e->local_offset = (long)local;
				z->count++;
			}
		}
		pos += 46 + nlen + elen + clen;
	}
	free(cd);

	if (z->count == 0) {
		fclose(f);
		return false;
	}
	strip_common_dir(z);
	z->file = f;
	return true;
}

void aszip_close(aszip_t *z) {
	if (z->file) {
		fclose(z->file);
		z->file = NULL;
	}
	z->count = 0;
}

int aszip_find(const aszip_t *z, const char *name) {
	for (int i = 0; i < z->count; i++) {
		if (strcmp(z->entry[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

uint8_t *aszip_read(const aszip_t *z, int index, size_t *out_len) {
	if (out_len) {
		*out_len = 0;
	}
	if (!z->file || index < 0 || index >= z->count) {
		return NULL;
	}
	const aszip_entry_t *e = &z->entry[index];
	if (e->comp_size <= 0 || e->uncomp_size <= 0) {
		return NULL;
	}

	// The data begins after the local header, whose name and extra field are
	// written separately from the central directory's and may differ.
	uint8_t lh[30];
	if (!read_at(z->file, e->local_offset, lh, sizeof(lh)) || rd32(lh) != LOCAL_SIG) {
		return NULL;
	}
	long data = e->local_offset + 30 + (long)rd16(lh + 26) + (long)rd16(lh + 28);

	uint8_t *raw = malloc((size_t)e->comp_size);
	if (!raw) {
		return NULL;
	}
	if (!read_at(z->file, data, raw, (size_t)e->comp_size)) {
		free(raw);
		return NULL;
	}

	uint8_t *out = NULL;
	if (e->method == 0) {
		out = raw;
		raw = NULL;
		if (out_len) {
			*out_len = (size_t)e->comp_size;
		}
	} else {
		size_t plain_len = 0;
		out = tinfl_decompress_mem_to_heap(raw, (size_t)e->comp_size, &plain_len, 0);
		if (out && out_len) {
			*out_len = plain_len;
		}
	}
	free(raw);
	return out;
}
