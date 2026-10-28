#include "gunzip.h"

#include <stdlib.h>
#include <string.h>

// miniz's inflate, compiled into the addon that needs it. The player links its
// own copy for PNG (src/system/image/png_scaled.c), but an addon must not lean
// on it: a service client that suddenly needed a new helper in the player would
// mean a new firmware image for an addon, which is the thing addons exist to
// avoid. Everything here is static to the library's own symbol table anyway
// (RTLD_LOCAL).
#include "src/system/image/miniz/tinfl_impl.inc"

bool body_is_gzip(const uint8_t *data, size_t len) {
	return data && len >= 18 && data[0] == 0x1f && data[1] == 0x8b && data[2] == 8;
}

uint8_t *gunzip_body(const uint8_t *data, size_t len, size_t *out_len, size_t max_out) {
	if (out_len) {
		*out_len = 0;
	}
	if (!body_is_gzip(data, len)) {
		return NULL;
	}

	// RFC 1952: a ten-byte header, then optional fields named by its flags,
	// then a raw deflate stream, then a CRC-32 and the uncompressed size.
	uint8_t flags = data[3];
	size_t pos = 10;
	if (flags & 0x04) { // FEXTRA
		if (pos + 2 > len) {
			return NULL;
		}
		size_t extra = (size_t)data[pos] | ((size_t)data[pos + 1] << 8);
		pos += 2 + extra;
	}
	if (flags & 0x08) { // FNAME, NUL-terminated
		while (pos < len && data[pos] != 0) {
			pos++;
		}
		pos++;
	}
	if (flags & 0x10) { // FCOMMENT, NUL-terminated
		while (pos < len && data[pos] != 0) {
			pos++;
		}
		pos++;
	}
	if (flags & 0x02) { // FHCRC
		pos += 2;
	}
	if (pos + 8 > len) {
		return NULL; // header and trailer alone, no stream between them
	}

	size_t plain_len = 0;
	// Flag 0: raw deflate. tinfl stops at the final block, so the trailer's
	// eight bytes beyond it are never read.
	void *plain = tinfl_decompress_mem_to_heap(data + pos, len - pos - 8, &plain_len, 0);
	if (!plain) {
		return NULL;
	}
	if (plain_len > max_out) {
		free(plain);
		return NULL;
	}

	// The callers parse C strings: the NUL is part of the contract.
	uint8_t *out = realloc(plain, plain_len + 1);
	if (!out) {
		free(plain);
		return NULL;
	}
	out[plain_len] = 0;
	if (out_len) {
		*out_len = plain_len;
	}
	return out;
}
