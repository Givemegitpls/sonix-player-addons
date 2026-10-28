#ifndef GUNZIP_H
#define GUNZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A gzipped response body, inflated.
//
// The player asks for `Accept-Encoding: identity` (http.c), and most servers
// honour it. A reverse proxy in front of one need not: nginx is configured
// `gzip on` in a hundred guides, and it compresses what it decides to compress
// whatever the request says. What arrives then is not JSON but 1f 8b, the
// parser rejects it, and the only thing a service client can say is "the server
// answered 200" -- which names no problem at all. That is how the Subsonic
// addon met its first real server.
//
// miniz is already in the tree for PNG (src/system/image/png_scaled.c compiles
// its inflate in); this is the same code with a gzip header in front of it.

// True when `data` starts with the gzip magic and a deflate method.
bool body_is_gzip(const uint8_t *data, size_t len);

// Inflates a gzip body, header and trailer included, into a freshly malloc'd,
// NUL-terminated buffer of *out_len bytes (the NUL is not counted). NULL when
// the data is not gzip, is truncated, or would inflate past `max_out`.
uint8_t *gunzip_body(const uint8_t *data, size_t len, size_t *out_len, size_t max_out);

#endif /* GUNZIP_H */
