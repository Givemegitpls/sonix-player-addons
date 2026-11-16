#ifndef ADDONSTORE_ASZIP_H
#define ADDONSTORE_ASZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// A zip, as much of it as an addon release needs.
//
// A release asset is a zip made by `zip` on a CI runner: a central directory at
// the end, entries that are either stored or deflated, and no zip64. That is
// small enough to read without a library -- the inflate itself is miniz's,
// compiled in from the same copy common/gunzip.c uses (the player's tinfl is
// not exported, and an addon must not depend on the player for something it can
// carry).
//
// The archive is read in two passes, because a release may also be a source
// archive with one directory around everything:
//
//   1. the central directory names the entries and their sizes;
//   2. aszip_open() strips a single top-level directory when every entry shares
//      one, so "dino/addon.json" becomes "addon.json".
//
// Only names that are safe to join onto a target directory are kept: nothing
// absolute, no "..", no backslash. A name that fails is refused by the caller
// as a corrupt archive rather than sanitised -- a release that tries is not one
// to install.

#define ASZIP_MAX_ENTRIES 256
#define ASZIP_NAME_MAX 200
#define ASZIP_ENTRY_MAX (16L * 1024 * 1024) // one file, not the whole archive

typedef struct {
	char name[ASZIP_NAME_MAX]; // after the top-level directory was stripped
	long comp_size;
	long uncomp_size;
	int method;			 // 0 stored, 8 deflate
	long local_offset;	 // offset of the local file header
} aszip_entry_t;

typedef struct {
	FILE *file;
	aszip_entry_t entry[ASZIP_MAX_ENTRIES];
	int count;
	int bad; // entries refused for their name; install refuses the archive
} aszip_t;

// Opens and indexes `path`. False when the file is not a zip, is truncated, uses
// zip64 or a compression method other than stored/deflate, or has more entries
// than the array above.
bool aszip_open(aszip_t *z, const char *path);

void aszip_close(aszip_t *z);

// Index of the entry whose (stripped) name is exactly `name`, or -1.
int aszip_find(const aszip_t *z, const char *name);

// Inflates entry `index` into a freshly malloc'd buffer of *out_len bytes. NULL
// when it is a directory, larger than ASZIP_ENTRY_MAX, or corrupt. The caller
// frees it.
uint8_t *aszip_read(const aszip_t *z, int index, size_t *out_len);

#endif /* ADDONSTORE_ASZIP_H */
