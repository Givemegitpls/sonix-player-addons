#include "addonfs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// mkdir -p, for the one directory deep that every caller here needs.
static bool make_dir(const char *path) {
	if (mkdir(path, 0777) == 0) {
		return true;
	}
	return errno == EEXIST;
}

bool addon_cache_dir(const sonix_addon_host_t *host, char *out, size_t out_size) {
	if (!host || !host->dir || !host->dir[0] || !out || out_size < 2) {
		return false;
	}
	int written = snprintf(out, out_size, "%s/cache", host->dir);
	if (written < 0 || (size_t)written >= out_size) {
		fprintf(stderr, "addon: '%s/cache' does not fit in %zu bytes\n", host->dir, out_size);
		out[0] = '\0';
		return false;
	}
	if (!make_dir(out)) {
		fprintf(stderr, "addon: cannot create '%s': %s\n", out, strerror(errno));
		out[0] = '\0';
		return false;
	}
	return true;
}

bool addon_migrate_path(const char *old_path, const char *new_path) {
	if (!old_path || !new_path || !old_path[0] || !new_path[0]) {
		return false;
	}

	struct stat old_st;
	struct stat new_st;
	if (stat(old_path, &old_st) != 0) {
		return true; // nothing to move: either already migrated or never there
	}
	if (stat(new_path, &new_st) == 0) {
		// Both present. The migration already happened and something recreated
		// the old path -- an older build of this addon, a copy from a backup.
		// The addon's own copy wins; the leftover is the user's to remove.
		fprintf(stderr, "addon: '%s' left behind; '%s' is the one in use\n", old_path, new_path);
		return true;
	}

	// The parent of the destination: <addon>/cache is made by addon_cache_dir(),
	// but an index beside the settings file lands directly in <addon>/.
	char parent[512];
	snprintf(parent, sizeof(parent), "%s", new_path);
	char *slash = strrchr(parent, '/');
	if (slash && slash != parent) {
		*slash = '\0';
		make_dir(parent);
	}

	if (rename(old_path, new_path) == 0) {
		fprintf(stderr, "addon: moved '%s' to '%s'\n", old_path, new_path);
		return true;
	}

	// rename(2) is the same filesystem here (one vfat), so a failure is real:
	// a read-only card, or a directory being pulled. A copy would leave the two
	// halves of an index disagreeing, so the attempt simply repeats next start.
	fprintf(stderr, "addon: cannot move '%s' to '%s': %s\n", old_path, new_path, strerror(errno));
	return false;
}
